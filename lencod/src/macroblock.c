
/*!
 *************************************************************************************
 * \file macroblock.c
 *
 * \brief
 *    Process one macroblock
 *
 * \author
 *    Main contributors (see contributors.h for copyright, address and affiliation details)
 *    - Inge Lille-Langoy               <inge.lille-langoy@telenor.com>
 *    - Rickard Sjoberg                 <rickard.sjoberg@era.ericsson.se>
 *    - Jani Lainema                    <jani.lainema@nokia.com>
 *    - Sebastian Purreiter             <sebastian.purreiter@mch.siemens.de>
 *    - Detlev Marpe                    <marpe@hhi.de>
 *    - Thomas Wedi                     <wedi@tnt.uni-hannover.de>
 *    - Ragip Kurceren                  <ragip.kurceren@nokia.com>
 *    - Alexis Michael Tourapis         <alexismt@ieee.org>
 *************************************************************************************
 */

#include "contributors.h"

#include <stdlib.h>
#include <assert.h>
#include <limits.h>
#include <memory.h>
#include <math.h>

#include "global.h"

#include "elements.h"
#include "macroblock.h"
#include "refbuf.h"
#include "fmo.h"
#include "vlc.h"
#include "image.h"
#include "mb_access.h"
#include "ratectl.h"              // header file for rate control
#include "rc_quadratic.h"
#include "cabac.h"
#include "transform8x8.h"
#include "transform.h"
#include "me_fullsearch.h"
#include "symbol.h"

#if TRACE
#define TRACE_SE(trace,str)  snprintf(trace,TRACESTRING_SIZE,str)
#else
#define TRACE_SE(trace,str)
#endif

extern const byte QP_SCALE_CR[52] ;

//Rate control
extern int delta_qp_mbaff[2][2],delta_qp_mbaff[2][2];
extern int qp_mbaff[2][2],qp_mbaff[2][2];

// function pointer for different ways of obtaining chroma interpolation
static void (*OneComponentChromaPrediction4x4) (imgpel* , int , int , short****** , int , short , int , int );
static void OneComponentChromaPrediction4x4_regenerate (imgpel* , int , int , short****** , int , short , int , int );
static void OneComponentChromaPrediction4x4_retrieve (imgpel* , int , int , short****** , int , short , int , int );

static int  slice_too_big(int rlc_bits);

static int  writeChromaIntraPredMode (Macroblock* currMB);
static int  writeMotionInfo2NAL      (Macroblock* currMB);
static int  writeChromaCoeff         (Macroblock* currMB);
static int  writeCBPandDquant        (Macroblock* currMB);

extern int *mvbits;

extern int QP2QUANT[40];
extern int ver_offset[4][8][4];
extern int hor_offset[4][8][4];

static int diff  [16];
static int diff64[64];
static imgpel l0_pred[MB_PIXELS];
static imgpel l1_pred[MB_PIXELS];
static const unsigned char subblk_offset_x[3][8][4] =
{
  { {0, 4, 0, 4},
    {0, 4, 0, 4},
    {0, 0, 0, 0},
    {0, 0, 0, 0},
    {0, 0, 0, 0},
    {0, 0, 0, 0},
    {0, 0, 0, 0},
    {0, 0, 0, 0}, },

  { {0, 4, 0, 4},
    {0, 4, 0, 4},
    {0, 4, 0, 4},
    {0, 4, 0, 4},
    {0, 0, 0, 0},
    {0, 0, 0, 0},
    {0, 0, 0, 0},
    {0, 0, 0, 0}, },

  { {0, 4, 0, 4},
    {8,12, 8,12},
    {0, 4, 0, 4},
    {8,12, 8,12},
    {0, 4, 0, 4},
    {8,12, 8,12},
    {0, 4, 0, 4},
    {8,12, 8,12}  }
};

static const unsigned char subblk_offset_y[3][8][4] =
{
  { {0, 0, 4, 4},
    {0, 0, 4, 4},
    {0, 0, 0, 0},
    {0, 0, 0, 0},
    {0, 0, 0, 0},
    {0, 0, 0, 0},
    {0, 0, 0, 0},
    {0, 0, 0, 0}, },

  { {0, 0, 4, 4},
    {8, 8,12,12},
    {0, 0, 4, 4},
    {8, 8,12,12},
    {0, 0, 0, 0},
    {0, 0, 0, 0},
    {0, 0, 0, 0},
    {0, 0, 0, 0} },

 { {0, 0, 4, 4},
   {0, 0, 4, 4},
   {8, 8,12,12},
   {8, 8,12,12},
   {0, 0, 4, 4},
   {0, 0, 4, 4},
   {8, 8,12,12},
   {8, 8,12,12} }
};

extern ColocatedParams *Co_located;
extern ColocatedParams *Co_located_JV[MAX_PLANE];  //!< Co_located to be used during 4:4:4 independent mode encoding

 /*!
 ************************************************************************
 * \brief
 *    updates the coordinates for the next macroblock to be processed
 *
 * \param mb_addr
 *    macroblock address in scan order
 ************************************************************************
 */
void set_MB_parameters (int mb_addr)
{
  img->current_mb_nr = mb_addr;

  get_mb_block_pos(mb_addr, &img->mb_x, &img->mb_y);

  img->block_x = img->mb_x << 2;
  img->block_y = img->mb_y << 2;
  img->pix_x   = img->block_x << 2;
  img->pix_y   = img->block_y << 2;
  img->opix_x  = img->pix_x;

  if (img->MbaffFrameFlag)
  {
    if (img->mb_data[mb_addr].mb_field)
    {
      pCurImg    = (mb_addr & 0x01) ? imgY_org_bot  : imgY_org_top;
      pImgOrg[0] = pCurImg;
      if ((img->yuv_format != YUV400) && !IS_INDEPENDENT(input))
      {
        imgUV_org  = (mb_addr & 0x01) ? imgUV_org_bot : imgUV_org_top;
        pImgOrg[1] = imgUV_org[0];
        pImgOrg[2] = imgUV_org[1];
      }

      img->opix_y   = (img->mb_y >> 1 ) << 4;
      img->mb_data[mb_addr].list_offset = (mb_addr % 2) ? 4 : 2;
    }
    else
    {
      pCurImg    = imgY_org_frm;      
      pImgOrg[0] = imgY_org_frm;
      if ((img->yuv_format != YUV400) && !IS_INDEPENDENT(input))
      {
        imgUV_org  = imgUV_org_frm;
        pImgOrg[1] = imgUV_org_frm[0];
        pImgOrg[2] = imgUV_org_frm[1];
      }
      img->opix_y   = img->block_y << 2;
      img->mb_data[mb_addr].list_offset = 0;
    }
  }
  else
  {
    img->opix_y   = img->block_y << 2;
    img->mb_data[mb_addr].list_offset = 0;
  }

  if (img->yuv_format != YUV400)
  {
    img->pix_c_x = (img->mb_cr_size_x * img->pix_x) >> 4;
    img->pix_c_y = (img->mb_cr_size_y * img->pix_y) >> 4;

    img->opix_c_x = (img->mb_cr_size_x * img->opix_x) >> 4;
    img->opix_c_y = (img->mb_cr_size_y * img->opix_y) >> 4;
  }
  //  printf ("set_MB_parameters: mb %d,  mb_x %d,  mb_y %d\n", mb_addr, img->mb_x, img->mb_y);
}


/*!
 ************************************************************************
 * \brief
 *    updates the coordinates and statistics parameter for the
 *    next macroblock
 ************************************************************************
 */
void proceed2nextMacroblock(Macroblock *currMB)
{
#if TRACE
  int use_bitstream_backing = (input->slice_mode == FIXED_RATE || input->slice_mode == CALL_BACK);
#endif
  int*        bitCount = currMB->bitcounter;
  int i;

  if (bitCount[BITS_TOTAL_MB] > img->max_bitCount)
    printf("Warning!!! Number of bits (%d) of macroblock_layer() data seems to exceed defined limit (%d).\n", bitCount[BITS_TOTAL_MB],img->max_bitCount);

  // Update the statistics
  stats->bit_use_mb_type[img->type]     += bitCount[BITS_MB_MODE       ];
  stats->tmp_bit_use_cbp[img->type]     += bitCount[BITS_CBP_MB        ];
  stats->bit_use_coeffC[img->type]      += bitCount[BITS_COEFF_UV_MB   ];
  stats->bit_use_coeff[0][img->type]    += bitCount[BITS_COEFF_Y_MB    ];
  stats->bit_use_coeff[1][img->type]    += bitCount[BITS_COEFF_CB_MB   ]; 
  stats->bit_use_coeff[2][img->type]    += bitCount[BITS_COEFF_CR_MB   ]; 
  stats->bit_use_delta_quant[img->type] += bitCount[BITS_DELTA_QUANT_MB];

  if (IS_INTRA(currMB))
  {
    ++stats->intra_chroma_mode[currMB->c_ipred_mode];

    if ((currMB->cbp&15) != 0)
    {
      ++stats->mode_use_transform[currMB->luma_transform_size_8x8_flag][img->type][currMB->mb_type];
    }
  }

  ++stats->mode_use[img->type][currMB->mb_type];
  stats->bit_use_mode[img->type][currMB->mb_type]+= bitCount[BITS_INTER_MB];

  if (img->type != I_SLICE)
  {
    if (currMB->mb_type == P8x8)
    {
      for(i=0;i<4;i++)
      {
        if (currMB->b8mode[i] > 0)
          ++stats->mode_use[img->type][currMB->b8mode[i]];
        else
          ++stats->b8_mode_0_use[img->type][currMB->luma_transform_size_8x8_flag];

        if (currMB->b8mode[i]==4)
        {
          if ((currMB->luma_transform_size_8x8_flag && (currMB->cbp&15) != 0) || input->Transform8x8Mode == 2)
            ++stats->mode_use_transform[1][img->type][4];
          else
            ++stats->mode_use_transform[0][img->type][4];
        }
      }
    }
    else if (currMB->mb_type >= 0 && currMB->mb_type <=3 && ((currMB->cbp&15) != 0))
    {
      ++stats->mode_use_transform[currMB->luma_transform_size_8x8_flag][img->type][currMB->mb_type];
    }
  }

  // Statistics
  if ((img->type == P_SLICE)||(img->type==SP_SLICE) )
  {
    ++stats->quant0;
    stats->quant1 += currMB->qp;      // to find average quant for inter frames
  }
}

/*!
************************************************************************
* \brief
*    updates chroma QP according to luma QP and bit depth
************************************************************************
*/
void set_chroma_qp(Macroblock *currMB)
{
  int i;
  for (i=0; i<2; i++)
  {
    currMB->qpc[i] = iClip3 ( -img->bitdepth_chroma_qp_scale, 51, currMB->qp + img->chroma_qp_offset[i] );
    currMB->qpc[i] = currMB->qpc[i] < 0 ? currMB->qpc[i] : QP_SCALE_CR[currMB->qpc[i]];
  }
}

/*!
 ************************************************************************
 * \brief
 *    initializes the current macroblock
 *
 * \param mb_addr
 *    macroblock address in scan order
 * \param mb_field
 *    true for field macroblock coding
 ************************************************************************
 */
void start_macroblock(Macroblock **currMB, int mb_addr, int mb_field)
{
  int i,j,l;
  int use_bitstream_backing = (input->slice_mode == FIXED_RATE || input->slice_mode == CALL_BACK);
  Slice *curr_slice = img->currentSlice;
  DataPartition *dataPart;
  Bitstream *currStream;
  int prev_mb;

  *currMB = &img->mb_data[mb_addr];


  (*currMB)->mb_field = mb_field;

  enc_picture->mb_field[mb_addr] = mb_field;
  (*currMB)->is_field_mode = (img->field_picture || ( img->MbaffFrameFlag && (*currMB)->mb_field));

  set_MB_parameters (mb_addr);

  prev_mb = FmoGetPreviousMBNr(img->current_mb_nr);

  if ( input->ChromaMCBuffer )
    OneComponentChromaPrediction4x4 = OneComponentChromaPrediction4x4_retrieve;
  else
    OneComponentChromaPrediction4x4 = OneComponentChromaPrediction4x4_regenerate;

  if(use_bitstream_backing)
  {
    if ((!input->MbInterlace)||((mb_addr & 0x01)==0)) // KS: MB AFF -> store stream positions for 1st MB only
    {
      // Keep the current state of the bitstreams
      if(!img->cod_counter)
      {
        for (i=0; i<curr_slice->max_part_nr; i++)
        {
          dataPart = &(curr_slice->partArr[i]);
          currStream = dataPart->bitstream;
          currStream->stored_bits_to_go = currStream->bits_to_go;
          currStream->stored_byte_pos   = currStream->byte_pos;
          currStream->stored_byte_buf   = currStream->byte_buf;
          stats->stored_bit_slice       = stats->bit_slice;

          if (input->symbol_mode ==CABAC)
          {
            dataPart->ee_recode = dataPart->ee_cabac;
          }
        }
      }
    }
  }

  // Save the slice number of this macroblock. When the macroblock below
  // is coded it will use this to decide if prediction for above is possible
  (*currMB)->slice_nr = img->current_slice_nr;

  // Initialize delta qp change from last macroblock. Feature may be used for future rate control
  // Rate control
  (*currMB)->qpsp       = img->qpsp;
  if(input->RCEnable)
    rc_handle_mb( prev_mb, *currMB, curr_slice );
  else
  {
    Slice* currSlice = img->currentSlice;
    int new_qp = img->qp;

    if (prev_mb>-1)
    {
      (*currMB)->prev_qp = img->mb_data[prev_mb].qp;
      (*currMB)->prev_dqp = (img->mb_data[prev_mb].slice_nr == img->current_slice_nr) ? img->mb_data[prev_mb].delta_qp : 0;
    }
    else
    {
      (*currMB)->prev_qp = currSlice->qp;
      (*currMB)->prev_dqp = 0;
    }

    if (prev_mb < 0) //first macroblock (of slice)
    {
      (*currMB)->delta_qp = 0;
      (*currMB)->qp       = img->qp;
    }
    else
    {
      if (!((input->MbInterlace) && img->bot_MB)) //top macroblock
      {
        if (img->mb_data[prev_mb].prev_cbp == 1)
        {
          (*currMB)->delta_qp = 0;
          (*currMB)->qp       = img->qp;
        }
        else
        {
          (*currMB)->qp = img->mb_data[prev_mb].prev_qp;
          (*currMB)->delta_qp = (*currMB)->qp - img->mb_data[prev_mb].qp;
          img->qp = (*currMB)->qp;
        }
      }
      else //bottom macroblock
      {
        (*currMB)->delta_qp = 0;
        (*currMB)->qp       = img->qp;       // needed in loop filter (even if constant QP is used)
      }
    }

    (*currMB)->delta_qp = new_qp - (*currMB)->qp + (*currMB)->delta_qp;
    img->qp = (*currMB)->qp = new_qp;

    //(*currMB)->delta_qp = (*currMB)->qp - (*currMB)->prev_qp;
    delta_qp_mbaff[(*currMB)->mb_field][img->bot_MB] = (*currMB)->delta_qp;
    qp_mbaff      [(*currMB)->mb_field][img->bot_MB] = (*currMB)->qp;
  } 

  set_chroma_qp (*currMB);
  (*currMB)->qp_scaled[0] = (*currMB)->qp + img->bitdepth_luma_qp_scale - MIN_QP;
  (*currMB)->qp_scaled[1] = (*currMB)->qpc[0] + img->bitdepth_chroma_qp_scale;
  (*currMB)->qp_scaled[2] = (*currMB)->qpc[1] + img->bitdepth_chroma_qp_scale;
    
  select_dct(*currMB);

  // loop filter parameter
  if (active_pps->deblocking_filter_control_present_flag)
  {
    (*currMB)->LFDisableIdc    = img->LFDisableIdc;
    (*currMB)->LFAlphaC0Offset = img->LFAlphaC0Offset;
    (*currMB)->LFBetaOffset    = img->LFBetaOffset;
  }
  else
  {
    (*currMB)->LFDisableIdc    = 0;
    (*currMB)->LFAlphaC0Offset = 0;
    (*currMB)->LFBetaOffset    = 0;
  }

  // If MB is next to a slice boundary, mark neighboring blocks unavailable for prediction
  CheckAvailabilityOfNeighbors(*currMB);

  if (input->symbol_mode == CABAC)
    CheckAvailabilityOfNeighborsCABAC(*currMB);

  // Reset vectors and reference indices
  for (l=0; l<2; l++)
  {
    for (j=img->block_y; j < img->block_y + BLOCK_MULTIPLE; j++)
    {
      memset(&enc_picture->ref_idx[l][j][img->block_x], -1, BLOCK_MULTIPLE * sizeof(char));
      memset(enc_picture->mv [l][j][img->block_x], 0, 2 * BLOCK_MULTIPLE * sizeof(short));
      for (i=img->block_x; i < img->block_x + BLOCK_MULTIPLE; i++)
        enc_picture->ref_pic_id[l][j][i]= -1;
    }
  }

  // Reset syntax element entries in MB struct
  (*currMB)->mb_type      = 0;
  (*currMB)->cbp_blk      = 0;
  (*currMB)->cbp          = 0;  
  (*currMB)->c_ipred_mode = DC_PRED_8;

  cmp_cbp[0] = cmp_cbp[1] = curr_cbp[0] = curr_cbp[1] = 0;

  memset((*currMB)->cbp_bits    , 0, 3 * sizeof(int64));
  memset((*currMB)->cbp_bits_8x8, 0, 3 * sizeof(int64));


  memset ((*currMB)->mvd, 0, BLOCK_CONTEXT * sizeof(int));
  memset ((*currMB)->intra_pred_modes, DC_PRED, MB_BLOCK_PARTITIONS * sizeof(char)); // changing this to char would allow us to use memset
  memset ((*currMB)->intra_pred_modes8x8, DC_PRED, MB_BLOCK_PARTITIONS * sizeof(char));

  //initialize the whole MB as INTRA coded
  //Blocks are set to notINTRA in write_one_macroblock
  if (input->UseConstrainedIntraPred)
  {
    img->intra_block[img->current_mb_nr] = 1;
  }

  // Initialize bitcounters for this macroblock
  if(prev_mb < 0) // No slice header to account for
  {
    (*currMB)->bitcounter[BITS_HEADER] = 0;
  }
  else if ((*currMB)->slice_nr == img->mb_data[prev_mb].slice_nr) // current MB belongs to the
  // same slice as the last MB
  {
    (*currMB)->bitcounter[BITS_HEADER] = 0;
  }

  (*currMB)->bitcounter[BITS_MB_MODE       ] = 0;
  (*currMB)->bitcounter[BITS_INTER_MB      ] = 0;
  (*currMB)->bitcounter[BITS_CBP_MB        ] = 0;
  (*currMB)->bitcounter[BITS_COEFF_Y_MB    ] = 0;    
  (*currMB)->bitcounter[BITS_COEFF_UV_MB   ] = 0;    
  (*currMB)->bitcounter[BITS_COEFF_CB_MB   ] = 0; 
  (*currMB)->bitcounter[BITS_COEFF_CR_MB   ] = 0;
  (*currMB)->bitcounter[BITS_DELTA_QUANT_MB] = 0;

  if(input->SearchMode == FAST_FULL_SEARCH)
    ResetFastFullIntegerSearch ();

  // disable writing of trace file
#if TRACE
  curr_slice->partArr[0].bitstream->trace_enabled = FALSE;
  if (input->partition_mode)
  {
    curr_slice->partArr[1].bitstream->trace_enabled = FALSE;
    curr_slice->partArr[2].bitstream->trace_enabled = FALSE;
  }
#endif
}

/*!
 ************************************************************************
 * \brief
 *    terminates processing of the current macroblock depending
 *    on the chosen slice mode
 ************************************************************************
 */
void terminate_macroblock(Macroblock *currMB, 
                          Boolean *end_of_slice,      //!< returns true for last macroblock of a slice, otherwise false
                          Boolean *recode_macroblock  //!< returns true if max. slice size is exceeded an macroblock must be recoded in next slice
                          )
{
  int i;
  Slice *currSlice = img->currentSlice;
  SyntaxElement se;
  int *partMap = assignSE2partition[input->partition_mode];
  DataPartition *dataPart;
  Bitstream *currStream;
  int rlc_bits=0;
  int use_bitstream_backing = (input->slice_mode == FIXED_RATE || input->slice_mode == CALL_BACK);
  int new_slice;
  static int skip = FALSE;


  // if previous mb in the same slice group has different slice number as the current, it's the
  // the start of new slice
  new_slice=0;
  if ( (img->current_mb_nr==0) || (FmoGetPreviousMBNr(img->current_mb_nr)<0) )
    new_slice=1;
  else if( img->mb_data[FmoGetPreviousMBNr(img->current_mb_nr)].slice_nr != img->current_slice_nr )
    new_slice=1;

  *recode_macroblock=FALSE;

  switch(input->slice_mode)
  {
  case NO_SLICES:
    currSlice->num_mb++;
    *recode_macroblock = FALSE;
    if ((currSlice->num_mb) == (int)img->PicSizeInMbs) // maximum number of MBs reached
      *end_of_slice = TRUE;

    // if it's end of current slice group, slice ends too
    *end_of_slice = (Boolean) (*end_of_slice | (img->current_mb_nr == FmoGetLastCodedMBOfSliceGroup (FmoMB2SliceGroup (img->current_mb_nr))));

    break;
  case FIXED_MB:
    // For slice mode one, check if a new slice boundary follows
    currSlice->num_mb++;
    *recode_macroblock = FALSE;
    //! Check end-of-slice group condition first
    *end_of_slice = (Boolean) (img->current_mb_nr == FmoGetLastCodedMBOfSliceGroup (FmoMB2SliceGroup (img->current_mb_nr)));
    //! Now check maximum # of MBs in slice
    *end_of_slice = (Boolean) (*end_of_slice | (currSlice->num_mb >= input->slice_argument));

    break;

    // For slice modes two and three, check if coding of this macroblock
    // resulted in too many bits for this slice. If so, indicate slice
    // boundary before this macroblock and code the macroblock again
  case FIXED_RATE:
    // in case of skip MBs check if there is a slice boundary
    // only for CAVLC (img->cod_counter is always 0 in case of CABAC)
    if(img->cod_counter)
    {
      // write out the skip MBs to know how many bits we need for the RLC
      se.value1 = img->cod_counter;
      se.value2 = 0;
      se.type = SE_MBTYPE;
      dataPart = &(currSlice->partArr[partMap[se.type]]);

      TRACE_SE (se.tracestring, "mb_skip_run");
      writeSE_UVLC(&se, dataPart);
      rlc_bits=se.len;

      currStream = dataPart->bitstream;
      // save the bitstream as it would be if we write the skip MBs
      currStream->bits_to_go_skip  = currStream->bits_to_go;
      currStream->byte_pos_skip    = currStream->byte_pos;
      currStream->byte_buf_skip    = currStream->byte_buf;
      // restore the bitstream
      currStream->bits_to_go = currStream->stored_bits_to_go;
      currStream->byte_pos   = currStream->stored_byte_pos;
      currStream->byte_buf   = currStream->stored_byte_buf;
      skip = TRUE;
    }
    //! Check if the last coded macroblock fits into the size of the slice
    //! But only if this is not the first macroblock of this slice
    if (!new_slice)
    {
      if(slice_too_big(rlc_bits))
      {
        *recode_macroblock = TRUE;
        *end_of_slice      = TRUE;
      }
      else if(!img->cod_counter)
        skip = FALSE;
    }
    // maximum number of MBs

    // check if current slice group is finished
    if ((*recode_macroblock == FALSE) && (img->current_mb_nr == FmoGetLastCodedMBOfSliceGroup (FmoMB2SliceGroup (img->current_mb_nr))))
    {
      *end_of_slice = TRUE;
      if(!img->cod_counter)
        skip = FALSE;
    }

    //! (first MB OR first MB in a slice) AND bigger that maximum size of slice
    if (new_slice && slice_too_big(rlc_bits))
    {
      *end_of_slice = TRUE;
      if(!img->cod_counter)
        skip = FALSE;
    }
    if (!*recode_macroblock)
      currSlice->num_mb++;
    break;

  case  CALL_BACK:
    if (img->current_mb_nr > 0 && !new_slice)
    {
      if (currSlice->slice_too_big(rlc_bits))
      {
        *recode_macroblock = TRUE;
        *end_of_slice = TRUE;
      }
    }

    if ( (*recode_macroblock == FALSE) && (img->current_mb_nr == FmoGetLastCodedMBOfSliceGroup (FmoMB2SliceGroup (img->current_mb_nr))))
      *end_of_slice = TRUE;
    break;

  default:
    snprintf(errortext, ET_SIZE, "Slice Mode %d not supported", input->slice_mode);
    error(errortext, 600);
  }

  if (*recode_macroblock == TRUE)
  {
    // Restore everything
    for (i=0; i<currSlice->max_part_nr; i++)
    {
      dataPart = &(currSlice->partArr[i]);
      currStream = dataPart->bitstream;
      currStream->bits_to_go = currStream->stored_bits_to_go;
      currStream->byte_pos  = currStream->stored_byte_pos;
      currStream->byte_buf  = currStream->stored_byte_buf;
      stats->bit_slice      = stats->stored_bit_slice;

      if (input->symbol_mode == CABAC)
      {
        dataPart->ee_cabac = dataPart->ee_recode;
      }
    }
  }

  if (input->symbol_mode == CAVLC)
  {
    // Skip MBs at the end of this slice
    dataPart = &(currSlice->partArr[partMap[SE_MBTYPE]]);
    if(*end_of_slice == TRUE  && skip == TRUE)
    {
      // only for Slice Mode 2 or 3
      // If we still have to write the skip, let's do it!
      if(img->cod_counter && *recode_macroblock == TRUE) // MB that did not fit in this slice
      {
        // If recoding is true and we have had skip,
        // we have to reduce the counter in case of recoding
        img->cod_counter--;
        if(img->cod_counter)
        {
          se.value1 = img->cod_counter;
          se.value2 = 0;
          se.type = SE_MBTYPE;
#if TRACE
          snprintf(se.tracestring, TRACESTRING_SIZE, "Final MB runlength = %3d",img->cod_counter);
#endif
          writeSE_UVLC(&se, dataPart);
          rlc_bits=se.len;
          currMB->bitcounter[BITS_MB_MODE]+=rlc_bits;
          img->cod_counter = 0;
        }
      }
      else //! MB that did not fit in this slice anymore is not a Skip MB
      {
        currStream = dataPart->bitstream;
        // update the bitstream
        currStream->bits_to_go = currStream->bits_to_go_skip;
        currStream->byte_pos  = currStream->byte_pos_skip;
        currStream->byte_buf  = currStream->byte_buf_skip;

        // update the statistics
        img->cod_counter = 0;
        skip = FALSE;
      }
    }

    // Skip MBs at the end of this slice for Slice Mode 0 or 1
    if(*end_of_slice == TRUE && img->cod_counter && !use_bitstream_backing)
    {
      se.value1 = img->cod_counter;
      se.value2 = 0;
      se.type = SE_MBTYPE;

      TRACE_SE (se.tracestring, "mb_skip_run");
      writeSE_UVLC(&se, dataPart);

      rlc_bits=se.len;
      currMB->bitcounter[BITS_MB_MODE]+=rlc_bits;
      img->cod_counter = 0;
    }
  }
}

/*!
 *****************************************************************************
 *
 * \brief
 *    For Slice Mode 2: Checks if one partition of one slice exceeds the
 *    allowed size
 *
 * \return
 *    FALSE if all Partitions of this slice are smaller than the allowed size
 *    TRUE is at least one Partition exceeds the limit
 *
 * \par Side effects
 *    none
 *
 * \date
 *    4 November 2001
 *
 * \author
 *    Tobias Oelbaum      drehvial@gmx.net
 *****************************************************************************/

int slice_too_big(int rlc_bits)
{
  Slice *currSlice = img->currentSlice;
  DataPartition *dataPart;
  Bitstream *currStream;
  EncodingEnvironmentPtr eep;
  int i;
  int size_in_bytes;

  //! CAVLC
  if (input->symbol_mode == CAVLC)
  {
    for (i=0; i<currSlice->max_part_nr; i++)
    {
      dataPart = &(currSlice->partArr[i]);
      currStream = dataPart->bitstream;
      size_in_bytes = currStream->byte_pos /*- currStream->tmp_byte_pos*/;

      if (currStream->bits_to_go < 8)
        size_in_bytes++;
      if (currStream->bits_to_go < rlc_bits)
        size_in_bytes++;
      if(size_in_bytes > input->slice_argument)
        return TRUE;
    }
  }

  //! CABAC
  if (input->symbol_mode ==CABAC)
  {
    for (i=0; i<currSlice->max_part_nr; i++)
    {
      dataPart= &(currSlice->partArr[i]);
      eep = &(dataPart->ee_cabac);

      if( arienco_bits_written(eep) > (input->slice_argument*8))
        return TRUE;
    }
  }
  return FALSE;
}


/*!
 ************************************************************************
 * \brief
 *    Predict Luma block
 ************************************************************************
 */
void OneComponentLumaPrediction ( imgpel*   mpred,       //!< array of prediction values (row by row)
                                  int    pic_pix_x,      //!< motion shifted horizontal coordinate of block
                                  int    pic_pix_y,      //!< motion shifted vertical   coordinate of block
                                  int    block_size_x,   //!< horizontal block size
                                  int    block_size_y,   //!< vertical block size
                                  StorablePicture *list //!< reference picture list
                                 )
{
  int     j;
  imgpel *ref_line = UMVLine4X (list->p_curr_img_sub, pic_pix_y, pic_pix_x);

  width_pad  = list->size_x_pad;
  height_pad = list->size_y_pad;
  
  for (j = 0; j < block_size_y; j++) 
  {
    memcpy(mpred, ref_line, block_size_x * sizeof(imgpel));
    ref_line += img_padded_size_x;
    mpred += block_size_x;
  }  
}


/*!
 ************************************************************************
 * \brief
 *    Predict one Luma block
 ************************************************************************
 */
void LumaPrediction ( Macroblock* currMB,//!< Current Macroblock
                     int   block_x,     //!< relative horizontal block coordinate of block
                     int   block_y,     //!< relative vertical   block coordinate of block
                     int   block_size_x,//!< relative horizontal block coordinate of block
                     int   block_size_y,//!< relative vertical   block coordinate of block
                     int   p_dir,       //!< prediction direction (0=list0, 1=list1, 2=bipred)
                     int   l0_mode,     //!< list0 prediction mode (1-7, 0=DIRECT if l1_mode=0)
                     int   l1_mode,     //!< list1 prediction mode (1-7, 0=DIRECT if l0_mode=0)
                     short l0_ref_idx,  //!< reference frame for list0 prediction (-1: Intra4x4 pred. with l0_mode)
                     short l1_ref_idx   //!< reference frame for list1 prediction 
                     )
{
  int  i, j;
  int  block_x4     = block_x + block_size_x;
  int  block_y4     = block_y + block_size_y;
  int  pic_opix_x   = ((img->opix_x + block_x) << 2) + IMG_PAD_SIZE_TIMES4;
  int  pic_opix_y   = ((img->opix_y + block_y) << 2) + IMG_PAD_SIZE_TIMES4;
  int  bx           = block_x >> 2;
  int  by           = block_y >> 2;
  imgpel* l0pred    = l0_pred;
  imgpel* l1pred    = l1_pred;  
  short**** mv_array = img->all_mv[by][bx];
  imgpel (*curr_mpr)[16] = img->mpr[0];

  int  apply_weights = ( (active_pps->weighted_pred_flag  && (img->type== P_SLICE || img->type == SP_SLICE)) ||
    (active_pps->weighted_bipred_idc && (img->type== B_SLICE)));

  if (currMB->bi_pred_me && l0_ref_idx == 0 && l1_ref_idx == 0 && p_dir == 2 && l0_mode==1 && l1_mode==1)
    mv_array = currMB->bi_pred_me == 1? img->bipred_mv1[by][bx] : img->bipred_mv2[by][bx];

  switch (p_dir)
  {
  case 0:
    OneComponentLumaPrediction (l0_pred, pic_opix_x + mv_array[LIST_0][l0_ref_idx][l0_mode][0], pic_opix_y + mv_array[LIST_0][l0_ref_idx][l0_mode][1], block_size_x, block_size_y, listX[0+currMB->list_offset][l0_ref_idx]);
    break;
  case 1:
    OneComponentLumaPrediction (l1_pred, pic_opix_x + mv_array[LIST_1][l1_ref_idx][l1_mode][0], pic_opix_y + mv_array[LIST_1][l1_ref_idx][l1_mode][1], block_size_x, block_size_y, listX[1+currMB->list_offset][l1_ref_idx]);
    break;
  case 2:
    OneComponentLumaPrediction (l0_pred, pic_opix_x + mv_array[LIST_0][l0_ref_idx][l0_mode][0], pic_opix_y + mv_array[LIST_0][l0_ref_idx][l0_mode][1], block_size_x, block_size_y, listX[0+currMB->list_offset][l0_ref_idx]);
    OneComponentLumaPrediction (l1_pred, pic_opix_x + mv_array[LIST_1][l1_ref_idx][l1_mode][0], pic_opix_y + mv_array[LIST_1][l1_ref_idx][l1_mode][1], block_size_x, block_size_y, listX[1+currMB->list_offset][l1_ref_idx]);
    break;
  default:
    break;
  }

  if (apply_weights)
  {
    if (p_dir==2)
    {
      int wbp0 = wbp_weight[0][l0_ref_idx][l1_ref_idx][0];
      int wbp1 = wbp_weight[1][l0_ref_idx][l1_ref_idx][0];
      int offset = (wp_offset[0][l0_ref_idx][0] + wp_offset[1][l1_ref_idx][0] + 1)>>1;
      int wp_round = 2*wp_luma_round;
      int weight_denom = luma_log_weight_denom + 1;
      for   (j=block_y; j<block_y4; j++)
        for (i=block_x; i<block_x4; i++)  
          curr_mpr[j][i] = iClip1( img->max_imgpel_value, 
          ((wbp0 * *l0pred++ + wbp1 * *l1pred++ + wp_round) >> (weight_denom)) + offset); 
    }
    else if (p_dir==0)
    {
      int wp = wp_weight[0][l0_ref_idx][0];
      int offset = wp_offset[0][l0_ref_idx][0];
      for   (j=block_y; j<block_y4; j++)
        for (i=block_x; i<block_x4; i++)
          curr_mpr[j][i] = iClip1( img->max_imgpel_value, 
          ((wp * *l0pred++  + wp_luma_round) >> luma_log_weight_denom) + offset);
    }
    else // (p_dir==1)
    {
      int wp = wp_weight[1][l1_ref_idx][0];
      int offset = wp_offset[1][l1_ref_idx][0];
      for   (j=block_y; j<block_y4; j++)
        for (i=block_x; i<block_x4; i++)
          curr_mpr[j][i] = iClip1( img->max_imgpel_value, 
          ((wp * *l1pred++  + wp_luma_round) >> luma_log_weight_denom) + offset );
    }
  }
  else
  {
    if (p_dir==2)
    {
      for   (j=block_y; j<block_y4; j++)
        for (i=block_x; i<block_x4; i++)
          curr_mpr[j][i] = (*l0pred++ + *l1pred++ + 1) >> 1;
    }
    else if (p_dir==0)
    {
      for (j=block_y; j<block_y4; j++)
      {
        memcpy(&(curr_mpr[j][block_x]), l0pred, block_size_x * sizeof(imgpel));
        l0pred += block_size_x;
      }
    }
    else // (p_dir==1)
    {
      for (j=block_y; j<block_y4; j++)
      {
        memcpy(&(curr_mpr[j][block_x]), l1pred, block_size_x * sizeof(imgpel));
        l1pred += block_size_x;
      }
    }
  }
}

/*!
 ************************************************************************
 * \brief
 *    Predict one Luma block
 ************************************************************************
 */
void LumaPredictionBi ( Macroblock* currMB, //!< Current Macroblock
                        int   block_x,      //!< relative horizontal block coordinate of 4x4 block
                        int   block_y,      //!< relative vertical   block coordinate of 4x4 block
                        int   block_size_x, //!< horizontal block size
                        int   block_size_y, //!< vertical   block size
                        int   l0_mode,      //!< list0 prediction mode (1-7, 0=DIRECT if l1_mode=0)
                        int   l1_mode,      //!< list1 prediction mode (1-7, 0=DIRECT if l0_mode=0)
                        short l0_ref_idx,   //!< reference frame for list0 prediction (-1: Intra4x4 pred. with l0_mode)
                        short l1_ref_idx,   //!< reference frame for list1 prediction 
                        int   list          //!< current list for prediction.
                        )
{
  int  i, j;
  int  block_x4  = block_x + block_size_x;
  int  block_y4  = block_y + block_size_y;
  int  pic_opix_x = ((img->opix_x + block_x) << 2) + IMG_PAD_SIZE_TIMES4;
  int  pic_opix_y = ((img->opix_y + block_y) << 2) + IMG_PAD_SIZE_TIMES4;
  int  bx        = block_x >> 2;
  int  by        = block_y >> 2;
  imgpel* l0pred     = l0_pred;
  imgpel* l1pred     = l1_pred;

  int  apply_weights = ( (active_pps->weighted_pred_flag && (img->type == P_SLICE || img->type == SP_SLICE)) ||
    (active_pps->weighted_bipred_idc && (img->type == B_SLICE)));  
  short   ****mv_array = list ? img->bipred_mv1[by][bx] : img->bipred_mv2[by][bx];
  imgpel (*curr_mpr)[16] = img->mpr[0];

  OneComponentLumaPrediction (l0_pred, pic_opix_x + mv_array[LIST_0][l0_ref_idx][l0_mode][0], pic_opix_y + mv_array[LIST_0][l0_ref_idx][l0_mode][1], block_size_x, block_size_y, listX[0+currMB->list_offset][l1_ref_idx]);
  OneComponentLumaPrediction (l1_pred, pic_opix_x + mv_array[LIST_1][l1_ref_idx][l1_mode][0], pic_opix_y + mv_array[LIST_1][l1_ref_idx][l1_mode][1], block_size_x, block_size_y, listX[1+currMB->list_offset][l1_ref_idx]);

  if (apply_weights)
  {
    int wbp0 = wbp_weight[0][l0_ref_idx][l1_ref_idx][0];
    int wbp1 = wbp_weight[1][l0_ref_idx][l1_ref_idx][0];
    int offset = (wp_offset[0][l0_ref_idx][0] + wp_offset[1][l1_ref_idx][0] + 1)>>1;
    int wp_round = 2*wp_luma_round;
    int weight_denom = luma_log_weight_denom + 1;

    for   (j=block_y; j<block_y4; j++)
      for (i=block_x; i<block_x4; i++)
        curr_mpr[j][i] = iClip1( img->max_imgpel_value,
        ((wbp0 * *l0pred++ + wbp1 * *l1pred++ + wp_round) >> weight_denom) + offset);
  }
  else
  {
    for   (j=block_y; j<block_y4; j++)
      for (i=block_x; i<block_x4; i++)
        curr_mpr[j][i] = (*l0pred++ + *l1pred++ + 1) >> 1;
  }
}


/*!
 ************************************************************************
 * \brief
 *    Residual Coding of an 8x8 Luma block (not for intra)
 *
 * \return
 *    coefficient cost
 ************************************************************************
 */
int LumaResidualCoding8x8 ( Macroblock* currMB, //!< Current Macroblock to be coded
                            int   *cbp,         //!< Output: cbp (updated according to processed 8x8 luminance block)
                            int64 *cbp_blk,     //!< Output: block cbp (updated according to processed 8x8 luminance block)
                            int   block8x8,     //!< block number of 8x8 block
                            short p_dir,        //!< prediction direction
                            int   l0_mode,      //!< list0 prediction mode (1-7, 0=DIRECT)
                            int   l1_mode,      //!< list1 prediction mode (1-7, 0=DIRECT)
                            short l0_ref_idx,   //!< reference picture for list0 prediction
                            short l1_ref_idx    //!< reference picture for list0 prediction
                           )
{
  int    block_y, block_x, pic_pix_y, pic_pix_x, i, j, nonzero = 0, cbp_blk_mask;
  int    coeff_cost = 0;
  int    mb_y       = (block8x8 >> 1) << 3;
  int    mb_x       = (block8x8 & 0x01) << 3;
  int    cbp_mask   = 1 << block8x8;
  int    bxx, byy;                   // indexing curr_blk
  int    skipped    = (l0_mode == 0 && l1_mode == 0 && (img->type != B_SLICE));
   
  //set transform size
  int    need_8x8_transform = currMB->luma_transform_size_8x8_flag;
  imgpel *imgOrg, *imgPred;
  int    *m7;
  imgpel (*curr_mpr)[16] = img->mpr[0];
  int uv, nonzero1=0, nonzero2=0;  
  coeff_cost1 = coeff_cost2 = 0;

  //===== loop over 4x4 blocks =====
  if(!need_8x8_transform)
  {
    for (byy=0, block_y=mb_y; block_y<mb_y+8; byy+=4, block_y+=4)
    {
      for (bxx=0, block_x=mb_x; block_x<mb_x+8; bxx+=4, block_x+=4)
      {
        pic_pix_x = img->opix_x + block_x;

        cbp_blk_mask = (block_x>>2) + block_y;

        //===== prediction of 4x4 block =====
        LumaPrediction (currMB, block_x, block_y, 4, 4, p_dir, l0_mode, l1_mode, l0_ref_idx, l1_ref_idx);
        if( active_sps->chroma_format_idc==YUV444 && !IS_INDEPENDENT(input) ) 
        {
          select_plane(PLANE_U);
          ChromaPrediction (currMB, 0, block_x, block_y, 4, 4, p_dir, l0_mode, l1_mode, l0_ref_idx, l1_ref_idx);
          select_plane(PLANE_V);
          ChromaPrediction (currMB, 1, block_x, block_y, 4, 4, p_dir, l0_mode, l1_mode, l0_ref_idx, l1_ref_idx);
          select_plane(PLANE_Y);
        }        

        //===== get displaced frame difference ======
        for (j = block_y; j < block_y + 4; j++)
        {
          imgOrg = &pCurImg[img->opix_y + j][pic_pix_x];    
          imgPred = &curr_mpr[j][block_x];
          m7 = &img->m7[0][j][block_x]; 
          for (i = 0; i < 4; i++)
          {
            *m7++ = *imgOrg++ - *imgPred++;
          }
        }

        if ( active_sps->chroma_format_idc==YUV444 && (IS_INDEPENDENT(input)==0) ) 
        {          
          for (uv=0; uv<2; uv++)
          {
            for (j = block_y; j < block_y + 4; j++)
            {
              imgOrg  = &imgUV_org[uv][img->opix_y + j][pic_pix_x];    
              imgPred = &img->mpr[uv + 1][j][block_x];
              m7 = &img->m7[uv+1][j][block_x]; 
              for (i=0; i<4; i++)
              {
                *m7++ = *imgOrg++ - *imgPred++;
              }
            }
          }
        }

        //===== DCT, Quantization, inverse Quantization, IDCT, Reconstruction =====
        if ( (img->NoResidueDirect != 1 && !skipped  ) ||
          ((currMB->qp_scaled[0])==0 && img->lossless_qpprime_flag==1) )
        {
          //===== DCT, Quantization, inverse Quantization, IDCT, Reconstruction =====
          //select_dct(currMB);
          nonzero = pDCT_4x4 (currMB, PLANE_Y, block_x, block_y, &coeff_cost, 0);

          if (nonzero)
          {
            (*cbp_blk) |= (int64)1 << cbp_blk_mask;  // one bit for every 4x4 block
            (*cbp)     |= cbp_mask;           // one bit for the 4x4 blocks of an 8x8 block
          }
          if( (active_sps->chroma_format_idc) == YUV444 && !IS_INDEPENDENT(input) ) 
          {
            if (img->type!=SP_SLICE)  
            {
              select_plane(PLANE_U);
              nonzero1 = pDCT_4x4( currMB, PLANE_U, block_x, block_y, &coeff_cost1, 0);
              select_plane(PLANE_V);
              nonzero2 = pDCT_4x4( currMB, PLANE_V, block_x, block_y, &coeff_cost2, 0);
              select_plane(PLANE_Y);
            }
            else
            {
              assert(img->type==SP_SLICE);   //SP_SLICE not implementd for FREXT_AD444
            }

            if (nonzero1)
            {
              (cur_cbp_blk[1]) |= (int64) 1 << cbp_blk_mask;  // one bit for every 4x4 block
              (cmp_cbp[0]) |= cbp_mask;           // one bit for the 4x4 blocks of an 8x8 block
            }
            if (nonzero2)
            {
              (cur_cbp_blk[2]) |= (int64)1 << cbp_blk_mask;  // one bit for every 4x4 block
              (cmp_cbp[1]) |= cbp_mask;           // one bit for the 4x4 blocks of an 8x8 block
            }   
          }
        }
      }
    }
  }
  else
  {
    for (block_y = mb_y; block_y < mb_y + 8; block_y += 8)
    {
      pic_pix_y = img->opix_y + block_y;

      for (block_x = mb_x; block_x < mb_x + 8; block_x += 8)
      {
        pic_pix_x = img->opix_x + block_x;

        cbp_blk_mask = (block_x>>2) + block_y;

        //===== prediction of 4x4 block =====
        LumaPrediction (currMB, block_x, block_y, 8, 8, p_dir, l0_mode, l1_mode, l0_ref_idx, l1_ref_idx);
        if(  (active_sps->chroma_format_idc) == YUV444 && !IS_INDEPENDENT(input) ) 
        {
          select_plane(PLANE_U);
          ChromaPrediction (currMB, 0, block_x, block_y, 8, 8, p_dir, l0_mode, l1_mode, l0_ref_idx, l1_ref_idx);
          select_plane(PLANE_V);
          ChromaPrediction (currMB, 1, block_x, block_y, 8, 8, p_dir, l0_mode, l1_mode, l0_ref_idx, l1_ref_idx);
          select_plane(PLANE_Y);
        }
        //===== get displaced frame difference ======
        for (j=0; j<8; j++)
        {
          imgOrg = &pCurImg[pic_pix_y + j][pic_pix_x];    
          imgPred = &curr_mpr[block_y + j][block_x];
          m7 = &img->m7[0][block_y + j][block_x];
          for (i=0; i<8; i++)
          {
            *m7++ = *imgOrg++ - *imgPred++;
          }
        }

        if ( (active_sps->chroma_format_idc == YUV444) && !IS_INDEPENDENT(input) ) 
        {
          for (uv=0; uv<2; uv++)
          {
            for (j=0; j<8; j++)
            {
              imgOrg = &imgUV_org[uv][pic_pix_y + j][pic_pix_x];    
              imgPred = &img->mpr[uv + 1][block_y + j][block_x];
              m7 = &img->m7[uv+1][block_y + j][block_x];
              for (i=0; i<8; i++)
              {
                *m7++ = *imgOrg++ - *imgPred++;
              }
            }
          }
        }
      }
    }
    if (img->NoResidueDirect != 1 && !skipped)
    {
      if (img->type!=SP_SLICE)
        nonzero = dct_8x8 (currMB, PLANE_Y, block8x8, &coeff_cost, 0);

      if (nonzero)
      {
        (*cbp_blk) |= 51 << (4*block8x8 - 2*(block8x8 & 0x01)); // corresponds to 110011, as if all four 4x4 blocks contain coeff, shifted to block position
        (*cbp)     |= cbp_mask;                               // one bit for the 4x4 blocks of an 8x8 block
      }
      if( (active_sps->chroma_format_idc == YUV444) && (IS_INDEPENDENT(input)==0) ) 
      {
        if (img->type!=SP_SLICE)
        {
          select_plane(PLANE_U);
          nonzero1 = dct_8x8( currMB, PLANE_U, block8x8, &coeff_cost1, 0);
          select_plane(PLANE_V);
          nonzero2 = dct_8x8( currMB, PLANE_V, block8x8, &coeff_cost2, 0);
          if (nonzero1)
          {
            (cur_cbp_blk[1]) |= 51 << (4*block8x8-2*(block8x8 & 0x01)); // corresponds to 110011, as if all four 4x4 blocks contain coeff, shifted to block position
            (cmp_cbp[0])     |= cbp_mask;           // one bit for the 4x4 blocks of an 8x8 block
          }
          if (nonzero2)
          {
            (cur_cbp_blk[2]) |= 51 << (4*block8x8-2*(block8x8 & 0x01)); // corresponds to 110011, as if all four 4x4 blocks contain coeff, shifted to block position
            (cmp_cbp[1]) |= cbp_mask;           // one bit for the 4x4 blocks of an 8x8 block
          }
        }
        select_plane(PLANE_Y);
      }
    }
  }

  /*
  The purpose of the action below is to prevent that single or 'expensive' coefficients are coded.
  With 4x4 transform there is larger chance that a single coefficient in a 8x8 or 16x16 block may be nonzero.
  A single small (level=1) coefficient in a 8x8 block will cost: 3 or more bits for the coefficient,
  4 bits for EOBs for the 4x4 blocks,possibly also more bits for CBP.  Hence the total 'cost' of that single
  coefficient will typically be 10-12 bits which in a RD consideration is too much to justify the distortion improvement.
  The action below is to watch such 'single' coefficients and set the reconstructed block equal to the prediction according
  to a given criterium.  The action is taken only for inter luma blocks.

  Notice that this is a pure encoder issue and hence does not have any implication on the standard.
  coeff_cost is a parameter set in dct_4x4() and accumulated for each 8x8 block.  If level=1 for a coefficient,
  coeff_cost is increased by a number depending on RUN for that coefficient.The numbers are (see also dct_4x4()): 3,2,2,1,1,1,0,0,...
  when RUN equals 0,1,2,3,4,5,6, etc.
  If level >1 coeff_cost is increased by 9 (or any number above 3). The threshold is set to 3. This means for example:
  1: If there is one coefficient with (RUN,level)=(0,1) in a 8x8 block this coefficient is discarded.
  2: If there are two coefficients with (RUN,level)=(1,1) and (4,1) the coefficients are also discarded
  sum_cnt_nonz[0] is the accumulation of coeff_cost over a whole macro block.  If sum_cnt_nonz[0] is 5 or less for the whole MB,
  all nonzero coefficients are discarded for the MB and the reconstructed block is set equal to the prediction.
  */

  if (img->NoResidueDirect != 1 && !skipped && coeff_cost <= _LUMA_COEFF_COST_ &&
    ((currMB->qp_scaled[0])!=0 || img->lossless_qpprime_flag==0)&&
    !(img->type==SP_SLICE && (si_frame_indicator==1 || sp2_frame_indicator==1 )))// last set of conditions
    // cannot skip when perfect reconstruction is as in switching pictures or SI pictures
  {
    coeff_cost  = 0;
    (*cbp)     &=  (63 - cbp_mask);
    (*cbp_blk) &= ~(51 << (4*block8x8-2*(block8x8 & 0x01)));
    /*
    for( i = 0; i < 4; i++ )
      for( j = 0; j < 2; j++ )
        memset( img->cofAC[block8x8][i][j], 0, 65 * sizeof(int));
    */
    for (j=mb_y; j<mb_y+8; j++)
      memcpy(&enc_picture->imgY[img->pix_y + j][img->pix_x + mb_x], &curr_mpr[j][mb_x], 2 * BLOCK_SIZE * sizeof(imgpel));

    if (img->type==SP_SLICE)
    {
      for (i=mb_x; i < mb_x + BLOCK_SIZE_8x8; i+=BLOCK_SIZE)
        for (j=mb_y; j < mb_y + BLOCK_SIZE_8x8; j+=BLOCK_SIZE)
          copyblock_sp(currMB, PLANE_Y, i, j);
    }
  }
  if( (active_sps->chroma_format_idc == YUV444) && (IS_INDEPENDENT(input)==0) )
  {
    if (img->NoResidueDirect != 1 && !skipped && coeff_cost1 <= _LUMA_COEFF_COST_ &&
      (currMB->qp_scaled[1]!=0 || img->lossless_qpprime_flag==0))// last set of conditions
    {
      coeff_cost1 = 0;
      cmp_cbp[0] &= (63 - cbp_mask);
      cur_cbp_blk[1] &= ~(51 << (4*block8x8-2*(block8x8 & 0x01)));

      for( i = 0; i < 4; i++ )
        for( j = 0; j < 2; j++ )
          memset( img->cofAC[block8x8+4][i][j], 0, 65 * sizeof(int));

      for (j=mb_y; j<mb_y+8; j++)
        memcpy(&enc_picture->imgUV[0][img->pix_y + j][img->pix_x + mb_x], &img->mpr[1][j][mb_x], 2 * BLOCK_SIZE * sizeof(imgpel));
    }

    if (img->NoResidueDirect != 1 && !skipped && coeff_cost2 <= _LUMA_COEFF_COST_ &&
      (currMB->qp_scaled[2]!=0 || img->lossless_qpprime_flag==0))// last set of conditions
    {
      coeff_cost2 = 0;
      cmp_cbp[1] &= (63 - cbp_mask);
      cur_cbp_blk[2] &= ~(51 << (4*block8x8-2*(block8x8 & 0x01)));

      for( i = 0; i < 4; i++ )
        for( j = 0; j < 2; j++ )
          memset( img->cofAC[block8x8+4+4][i][j], 0, 65 * sizeof(int));

      for (j=mb_y; j<mb_y+8; j++)
        memcpy(&enc_picture->imgUV[1][img->pix_y + j][img->pix_x + mb_x], &img->mpr[2][j][mb_x], 2 * BLOCK_SIZE * sizeof(imgpel));
    }
  }

  return coeff_cost;
}

/*!
 ************************************************************************
 * \brief
 *    Set mode parameters and reference frames for an 8x8 block
 ************************************************************************
 */
void SetModesAndRefframe (Macroblock* currMB, int b8, short* p_dir, int* l0_mode, int* l1_mode, short* l0_ref, short* l1_ref)
{
  int         j      = 2*(b8>>1);
  int         i      = 2*(b8 & 0x01);

  *l0_mode = *l1_mode = *l0_ref = *l1_ref = -1;

  *p_dir  = currMB->b8pdir[b8];

  if (img->type!=B_SLICE)
  {
    *l0_ref  = enc_picture->ref_idx[LIST_0][img->block_y+j][img->block_x+i];
    *l1_ref  = 0;
    *l0_mode = currMB->b8mode[b8];
    *l1_mode = 0;
  }
  else
  {
    if (currMB->b8pdir[b8]==-1)
    {
      *l0_ref  = -1;
      *l1_ref  = -1;
      *l0_mode =  0;
      *l1_mode =  0;
    }
    else if (currMB->b8pdir[b8]==0)
    {
      *l0_ref  = enc_picture->ref_idx[LIST_0][img->block_y+j][img->block_x+i];
      *l1_ref  = 0;
      *l0_mode = currMB->b8mode[b8];
      *l1_mode = 0;
    }
    else if (currMB->b8pdir[b8]==1)
    {
      *l0_ref  = 0;
      *l1_ref  = enc_picture->ref_idx[LIST_1][img->block_y+j][img->block_x+i];
      *l0_mode = 0;
      *l1_mode = currMB->b8mode[b8];
    }
    else
    {
      *l0_ref  = enc_picture->ref_idx[LIST_0][img->block_y+j][img->block_x+i];
      *l1_ref  = enc_picture->ref_idx[LIST_1][img->block_y+j][img->block_x+i];
      *l0_mode = currMB->b8mode[b8];
      *l1_mode = currMB->b8mode[b8];
    }
  }
}


/*!
 ************************************************************************
 * \brief
 *    Residual Coding of a Luma macroblock (not for intra)
 ************************************************************************
 */
void LumaResidualCoding (Macroblock *currMB)
{
  int i,j,block8x8,b8_x,b8_y;
  int l0_mode, l1_mode;
  short p_dir, refframe;
  int sum_cnt_nonz[3] = {0 ,0, 0};
  imgpel (*curr_mpr)[16] = img->mpr[0];

  currMB->cbp     = 0;
  currMB->cbp_blk = 0;
  //VGext
  cmp_cbp[0] = cmp_cbp[1] = 0;
  cur_cbp_blk[1] = cur_cbp_blk[2] = 0;
  //VGext-end

  for (block8x8=0; block8x8<4; block8x8++)
  {
    short l1_ref;
    SetModesAndRefframe (currMB, block8x8, &p_dir, &l0_mode, &l1_mode, &refframe, &l1_ref);
    
    sum_cnt_nonz[0] += LumaResidualCoding8x8 (currMB, &(currMB->cbp), &(currMB->cbp_blk), block8x8,
      p_dir, l0_mode, l1_mode, refframe, l1_ref);
    if(  (active_sps->chroma_format_idc == YUV444) && (IS_INDEPENDENT(input)==0)  ) 
    {
      sum_cnt_nonz[1] += coeff_cost1;
      sum_cnt_nonz[2] += coeff_cost2;
    }
  }

  if (sum_cnt_nonz[0] <= _LUMA_MB_COEFF_COST_ &&
    ((currMB->qp_scaled[0])!=0 || img->lossless_qpprime_flag==0) &&
    !(img->type==SP_SLICE && (si_frame_indicator==1 || sp2_frame_indicator==1)))// modif ES added last set of conditions
    //cannot skip if SI or switching SP frame perfect reconstruction is needed
  {
    currMB->cbp     &= 0xfffff0 ;
    currMB->cbp_blk &= 0xff0000 ;
    for (j=0; j < MB_BLOCK_SIZE; j++)
      memcpy(&enc_picture->imgY[img->pix_y+j][img->pix_x], curr_mpr[j], MB_BLOCK_SIZE * sizeof (imgpel));
    /*
    for(block8x8=0;block8x8<4;block8x8++)
    {
      for( i = 0; i < 4; i++ )
        for( j = 0; j < 2; j++ )
          memset( img->cofAC[block8x8][i][j], 0, 65 * sizeof(int));
    }
    */
    if (img->type==SP_SLICE)
    {
      for(block8x8=0;block8x8<4;block8x8++)
      {
        b8_x=(block8x8&1)<<3;
        b8_y=(block8x8&2)<<2;
        for (i = b8_x; i < b8_x + BLOCK_SIZE_8x8; i += 4)
          for (j = b8_y; j < b8_y + BLOCK_SIZE_8x8;j += 4)
            copyblock_sp(currMB, PLANE_Y, i, j);
      }
    }
  }

  if((img->yuv_format == YUV444) && !IS_INDEPENDENT(input) )
  {
    if(sum_cnt_nonz[1] <= _LUMA_MB_COEFF_COST_ &&
      ((currMB->qp_scaled[1])!=0 ||img->lossless_qpprime_flag==0)) 
    {
      cmp_cbp[0] &= 0xfffff0 ;
      cur_cbp_blk[1] &= 0xff0000 ;
      for (j=0; j < MB_BLOCK_SIZE; j++)
        memcpy(&enc_picture->p_img[1][img->pix_y+j][img->pix_x], img->mpr[1][j], MB_BLOCK_SIZE * sizeof (imgpel));

      for( block8x8 = 0; block8x8 < 4; block8x8++) 
        for( i = 0; i < 4; i++ )
          for( j = 0; j < 2; j++ )
            memset( img->cofAC[block8x8 + 4][i][j], 0, 65 * sizeof(int));
    }

    if (sum_cnt_nonz[2] <= _LUMA_MB_COEFF_COST_ &&
      ((currMB->qp_scaled[2])!=0 || img->lossless_qpprime_flag==0)) 
    {
      cmp_cbp[1] &= 0xfffff0 ;
      cur_cbp_blk[2] &= 0xff0000 ;
      for (j=0; j < MB_BLOCK_SIZE; j++)
        memcpy(&enc_picture->p_img[2][img->pix_y+j][img->pix_x], img->mpr[2][j], MB_BLOCK_SIZE * sizeof (imgpel));

      for( block8x8 = 0; block8x8 < 4; block8x8++)
        for( i = 0; i < 4; i++ )
          for( j = 0; j < 2; j++ )
            memset( img->cofAC[block8x8 + 8][i][j], 0, 65 * sizeof(int));
    }

    currMB->cbp |= cmp_cbp[0];
    currMB->cbp |= cmp_cbp[1];
  }
}


/*!
 ************************************************************************
 * \brief
 *    Makes the decision if 8x8 tranform will be used (for RD-off)
 ************************************************************************
 */
int TransformDecision (Macroblock *currMB, int block_check, int *cost)
{
  int    block_y, block_x, i, j, k;
  int    mb_y, mb_x, block8x8;
  int    l0_mode, l1_mode;
  short  p_dir, l0_ref, l1_ref;
  int    num_blks;
  int    cost8x8=0, cost4x4=0;
  int    *diff_ptr;
  imgpel (*curr_mpr)[16] = img->mpr[0];

  if(block_check==-1)
  {
    block8x8=0;
    num_blks=4;
  }
  else
  {
    block8x8=block_check;
    num_blks=block_check+1;
  }

  for (; block8x8<num_blks; block8x8++)
  {
    SetModesAndRefframe (currMB, block8x8, &p_dir, &l0_mode, &l1_mode, &l0_ref, &l1_ref);

    mb_y = (block8x8 >> 1) << 3;
    mb_x = (block8x8 & 0x01) << 3;
    //===== loop over 4x4 blocks =====
    k=0;
    for (block_y=mb_y; block_y<mb_y+8; block_y+=4)
    {     
      for (block_x=mb_x; block_x<mb_x+8; block_x+=4)
      {
        //===== prediction of 4x4 block =====
        LumaPrediction (currMB, block_x, block_y, 4, 4, p_dir, l0_mode, l1_mode, l0_ref, l1_ref);

        //===== get displaced frame difference ======
        diff_ptr=&diff64[k];
        for (j=block_y; j<block_y + 4; j++)
        {
          for (i = block_x; i < block_x + 4; i++, k++)
            diff64[k] = pCurImg[img->opix_y+j][img->opix_x + i] - curr_mpr[j][i];
        }
        cost4x4 += distortion4x4 (diff_ptr);
      }
    }
    cost8x8 += distortion8x8 (diff64);
  }

  if(input->Transform8x8Mode==2) //always allow 8x8 transform
    return 1;
  else if(cost8x8<cost4x4)
    return 1;
  else
  {
    *cost = (*cost-cost8x8+cost4x4);
    return 0;
  }
}

/*!
 ************************************************************************
 * \brief
 *    Predict (on-the-fly) one component of a chroma 4x4 block
 ************************************************************************
 */
void OneComponentChromaPrediction4x4_regenerate (
                                 imgpel*     mpred,      //!< array to store prediction values
                                 int         block_c_x,  //!< horizontal pixel coordinate of 4x4 block
                                 int         block_c_y,  //!< vertical   pixel coordinate of 4x4 block
                                 short****** mv,         //!< motion vector array
                                 int         list_idx,   //!< reference picture list
                                 short       ref,        //!< reference index
                                 int         blocktype,  //!< block type
                                 int         uv)         //!< chroma component
{
  int     i, j, ii, jj, ii0, jj0, ii1, jj1, if0, if1, jf0, jf1;
  short*  mvb;

  int     f1_x = 64/img->mb_cr_size_x;
  int     f2_x=f1_x-1;

  int     f1_y = 64/img->mb_cr_size_y;
  int     f2_y=f1_y-1;

  int     f3=f1_x*f1_y, f4=f3>>1;
  int     list_offset = img->mb_data[img->current_mb_nr].list_offset;
  int     max_y_cr = (int) (list_offset ? (img->height_cr >> 1) - 1 : img->height_cr - 1);
  int     max_x_cr = (int) (img->width_cr - 1);
  int     jjx, iix;
  int     mb_cr_y_div4 = img->mb_cr_size_y>>2;
  int     mb_cr_x_div4 = img->mb_cr_size_x>>2;
  int     jpos;

  StorablePicture **list = listX[list_idx + list_offset];

  imgpel** refimage = list[ref]->imgUV[uv];

  for (j=block_c_y; j < block_c_y + BLOCK_SIZE; j++)
  {
    jjx = j/mb_cr_y_div4;
    jpos = (j + img->opix_c_y)*f1_y;

    for (i=block_c_x; i < block_c_x + BLOCK_SIZE; i++)
    {
      iix = i/mb_cr_x_div4;
      mvb  = mv [jjx][iix][list_idx][ref][blocktype];

      ii   = (i + img->opix_c_x)*f1_x + mvb[0];
      jj   = jpos + mvb[1];

      if (active_sps->chroma_format_idc == 1)
        jj  += list[ref]->chroma_vector_adjustment;

      ii0  = iClip3 (0, max_x_cr, ii/f1_x);
      jj0  = iClip3 (0, max_y_cr, jj/f1_y);
      ii1  = iClip3 (0, max_x_cr, (ii+f2_x)/f1_x);
      jj1  = iClip3 (0, max_y_cr, (jj+f2_y)/f1_y);

      if1  = (ii&f2_x);  if0 = f1_x-if1;
      jf1  = (jj&f2_y);  jf0 = f1_y-jf1;

      *mpred++ = (if0 * jf0 * refimage[jj0][ii0] +
                  if1 * jf0 * refimage[jj0][ii1] +
                  if0 * jf1 * refimage[jj1][ii0] +
                  if1 * jf1 * refimage[jj1][ii1] + f4) / f3;
    }
  }
}

/*!
 ************************************************************************
 * \brief
 *    Retrieve one component of a chroma 4x4 block from the buffer
 ************************************************************************
 */
void OneComponentChromaPrediction4x4_retrieve (imgpel*        mpred,      //!< array to store prediction values
                                 int         block_c_x,  //!< horizontal pixel coordinate of 4x4 block
                                 int         block_c_y,  //!< vertical   pixel coordinate of 4x4 block
                                 short****** mv,         //!< motion vector array
                                 int         list_idx,   //!< reference picture list
                                 short       ref,        //!< reference index
                                 int         blocktype,  //!< block type
                                 int         uv)         //!< chroma component
{
  int     j, ii, jj;
  short*  mvb;

  int     list_offset = img->mb_data[img->current_mb_nr].list_offset;

  int     jjx;
  int     right_shift_x = 4 - chroma_shift_x;
  int     right_shift_y = 4 - chroma_shift_y;
  int     jpos;

  int     pos_x1 = block_c_x >> right_shift_x;
  int     pos_x2 = (block_c_x + 2) >> right_shift_x;
  int     ipos1 = ((block_c_x + img->opix_c_x) << chroma_shift_x) + IMG_PAD_SIZE_TIMES4;
  int     ipos2 = ((block_c_x + 2 + img->opix_c_x) << chroma_shift_x) + IMG_PAD_SIZE_TIMES4;


  StorablePicture **list = listX[list_idx + list_offset];

  imgpel**** refsubimage = list[ref]->imgUV_sub[uv];
  imgpel *line_ptr;

  int jj_chroma = ((active_sps->chroma_format_idc == 1) ? list[ref]->chroma_vector_adjustment : 0) + IMG_PAD_SIZE_TIMES4;

  width_pad_cr  = list[ref]->size_x_cr_pad;
  height_pad_cr = list[ref]->size_y_cr_pad;

  for (j=block_c_y; j < block_c_y + BLOCK_SIZE; j++)
  {
    jjx = j >> right_shift_y; // translate into absolute block (luma) coordinates

    jpos = ( (j + img->opix_c_y) << chroma_shift_y ) + jj_chroma;

    mvb  = mv [jjx][pos_x1][list_idx][ref][blocktype];

    ii   = ipos1 + mvb[0];
    jj   = jpos  + mvb[1];

    line_ptr = UMVLine8X_chroma ( refsubimage, jj, ii);
    *mpred++ = *line_ptr++;
    *mpred++ = *line_ptr;

    mvb  = mv [jjx][pos_x2][list_idx][ref][blocktype];

    ii   = ipos2 + mvb[0];
    jj   = jpos  + mvb[1];

    line_ptr = UMVLine8X_chroma ( refsubimage, jj, ii);
    *mpred++ = *line_ptr++;
    *mpred++ = *line_ptr;
  }
}


/*!
 ************************************************************************
 * \brief
 *    Retrieve one component of a chroma block from the buffer
 ************************************************************************
 */
void OneComponentChromaPrediction ( imgpel* mpred,      //!< array to store prediction values
                                   int    pic_pix_x,      //!< motion shifted horizontal coordinate of block
                                   int    pic_pix_y,      //!< motion shifted vertical  block
                                   int     block_size_x,   //!< horizontal block size
                                   int     block_size_y,   //!< vertical block size                                      
                                   StorablePicture *list, //!< reference picture list
                                   int         uv)         //!< chroma component
{
  int     j;
  imgpel *ref_line = UMVLine4X (list->imgUV_sub[uv], pic_pix_y, pic_pix_x);

  width_pad_cr  = list->size_x_cr_pad;
  height_pad_cr = list->size_y_cr_pad;

  for (j = 0; j < block_size_y; j++) 
  {
    memcpy(mpred, ref_line, block_size_x * sizeof(imgpel));
    ref_line += img_cr_padded_size_x;
    mpred += block_size_x;
  }
}

/*!
 ************************************************************************
 * \brief
 *    Predict an intra chroma 4x4 block
 ************************************************************************
 */
void IntraChromaPrediction4x4 (Macroblock* currMB, //! <-- Current Macroblock
                               int  uv,            //! <-- colour component
                               int  block_x,       //! <-- relative horizontal block coordinate of 4x4 block
                               int  block_y)       //! <-- relative vertical   block coordinate of 4x4 block
{
  int mode = currMB->c_ipred_mode;
  int j;
  imgpel (*curr_mpr)[16]           = img->mpr[ uv + 1];
  imgpel (*curr_mpr_16x16)[16][16] = img->mpr_16x16[uv + 1];

  //===== prediction =====
  for (j=block_y; j<block_y+4; j++)
    memcpy(&curr_mpr[j][block_x],&curr_mpr_16x16[mode][j][block_x], BLOCK_MULTIPLE * sizeof(imgpel));
}

/*!
 ************************************************************************
 * \brief
 *    Predict one chroma 4x4 block
 ************************************************************************
 */
void ChromaPrediction ( Macroblock* currMB, //!< Current Macroblock
                       int   uv,           // <-- colour component
                       int   block_x,      //!< relative horizontal block coordinate of block
                       int   block_y,      //!< relative vertical   block coordinate of block
                       int   block_size_x,//!< relative horizontal block coordinate of block
                       int   block_size_y,//!< relative vertical   block coordinate of block                        
                       int   p_dir,       //!< prediction direction (0=list0, 1=list1, 2=bipred)
                       int   l0_mode,      //!< list0  prediction mode (1-7, 0=DIRECT if l1_mode=0)
                       int   l1_mode,      //!< list1 prediction mode (1-7, 0=DIRECT if l0_mode=0)
                       short l0_ref_idx,   //!< reference frame for list0 prediction (if (<0) -> intra prediction)
                       short l1_ref_idx)   //!< reference frame for list1 prediction 
{
  int  i, j;
  int  block_x4     = block_x + block_size_x;
  int  block_y4     = block_y + block_size_y;
  int  pic_opix_x   = ((img->opix_c_x + block_x) << 2) + IMG_PAD_SIZE_TIMES4;
  int  pic_opix_y   = ((img->opix_c_y + block_y) << 2) + IMG_PAD_SIZE_TIMES4;
  int  bx           = block_x >> 2;
  int  by           = block_y >> 2;
  imgpel* l0pred     = l0_pred;
  imgpel* l1pred     = l1_pred;
  short**** mv_array = img->all_mv[by][bx];    
  int max_imgpel_value_uv = img->max_imgpel_value_comp[1];
  imgpel (*curr_mpr)[16] = img->mpr[ uv + 1];

  int  apply_weights = ( (active_pps->weighted_pred_flag && (img->type == P_SLICE || img->type == SP_SLICE)) ||
    (active_pps->weighted_bipred_idc && (img->type == B_SLICE)));

  if (currMB->bi_pred_me && l0_ref_idx == 0 && l1_ref_idx == 0 && p_dir == 2 && l0_mode==1 && l1_mode==1)
    mv_array = currMB->bi_pred_me == 1? img->bipred_mv1[by][bx] : img->bipred_mv2[by][bx];


  //===== INTRA PREDICTION =====
  if (p_dir==-1)
  {
    IntraChromaPrediction4x4 (currMB, uv, block_x, block_y);
    return;
  }

  //===== INTER PREDICTION =====
  switch (p_dir)
  {
  case 0:
    OneComponentChromaPrediction (l0_pred, pic_opix_x + mv_array[LIST_0][l0_ref_idx][l0_mode][0], pic_opix_y + mv_array[LIST_0][l0_ref_idx][l0_mode][1], block_size_x, block_size_y, listX[0+currMB->list_offset][l0_ref_idx], uv);
    break;
  case 1: 
    OneComponentChromaPrediction (l1_pred, pic_opix_x + mv_array[LIST_1][l1_ref_idx][l1_mode][0], pic_opix_y + mv_array[LIST_1][l1_ref_idx][l1_mode][1], block_size_x, block_size_y, listX[1+currMB->list_offset][l1_ref_idx], uv);
    break;
  case 2:
    OneComponentChromaPrediction (l0_pred, pic_opix_x + mv_array[LIST_0][l0_ref_idx][l0_mode][0], pic_opix_y + mv_array[LIST_0][l0_ref_idx][l0_mode][1], block_size_x, block_size_y, listX[0+currMB->list_offset][l0_ref_idx], uv);
    OneComponentChromaPrediction (l1_pred, pic_opix_x + mv_array[LIST_1][l1_ref_idx][l1_mode][0], pic_opix_y + mv_array[LIST_1][l1_ref_idx][l1_mode][1], block_size_x, block_size_y, listX[1+currMB->list_offset][l1_ref_idx], uv);
    break;
  default:
    break;
  }

  if (apply_weights)
  {
    if (p_dir==2)
    {
      int wbp0 = wbp_weight[0][l0_ref_idx][l1_ref_idx][uv+1];
      int wbp1 = wbp_weight[1][l0_ref_idx][l1_ref_idx][uv+1];
      int offset = (wp_offset[0][l0_ref_idx][uv+1] + wp_offset[1][l1_ref_idx][uv+1] + 1)>>1;
      int wp_round = 2*wp_chroma_round;
      int weight_denom = luma_log_weight_denom + 1;


      for   (j=block_y; j<block_y4; j++)
        for (i=block_x; i<block_x4; i++)
          curr_mpr[j][i] =  iClip1( max_imgpel_value_uv,
          ((wbp0 * *l0pred++ + wbp1 * *l1pred++ + wp_round) >> (weight_denom)) + (offset) );
    }
    else if (p_dir==0)
    {
      int wp = wp_weight[0][l0_ref_idx][uv+1];
      int offset = wp_offset[0][l0_ref_idx][uv+1];
      for   (j=block_y; j<block_y4; j++)
        for (i=block_x; i<block_x4; i++)
          curr_mpr[j][i] = iClip1( max_imgpel_value_uv, (( wp * *l0pred++ + wp_chroma_round) >> chroma_log_weight_denom) +  offset);
    }
    else // (p_dir==1)
    {
      int wp = wp_weight[1][l1_ref_idx][uv+1];
      int offset = wp_offset[1][l1_ref_idx][uv+1];

      for   (j=block_y; j<block_y4; j++)
        for (i=block_x; i<block_x4; i++)
          curr_mpr[j][i] = iClip1( max_imgpel_value_uv, ((wp * *l1pred++ + wp_chroma_round) >> chroma_log_weight_denom) + offset);
    }
  }
  else
  {
    if (p_dir==2)
    {
      for   (j=block_y; j<block_y4; j++)
        for (i=block_x; i<block_x4; i++)
          curr_mpr[j][i] = (*l0pred++ + *l1pred++ + 1) >> 1;
    }
    else if (p_dir==0)
    {
      for (j=block_y; j<block_y4; j++)
      {
        memcpy(&(curr_mpr[j][block_x]), l0pred, block_size_x * sizeof(imgpel));
        l0pred += block_size_x;
      }
    }
    else // (p_dir==1)
    {
      for (j=block_y; j<block_y4; j++)
      {
        memcpy(&(curr_mpr[j][block_x]), l1pred, block_size_x * sizeof(imgpel));
        l1pred += block_size_x;
      }
    }
  }
}


/*!
 ************************************************************************
 * \brief
 *    Predict one chroma 4x4 block
 ************************************************************************
 */
void ChromaPrediction4x4 ( Macroblock* currMB, // <-- Current Macroblock
                           int   uv,           // <-- colour component
                           int   block_x,      // <-- relative horizontal block coordinate of 4x4 block
                           int   block_y,      // <-- relative vertical   block coordinate of 4x4 block
                           int   p_dir,        // <-- prediction direction
                           int   l0_mode,      // <-- list0  prediction mode (1-7, 0=DIRECT if l1_mode=0)
                           int   l1_mode,      // <-- list1 prediction mode (1-7, 0=DIRECT if l0_mode=0)
                           short l0_ref_idx,   // <-- reference frame for list0 prediction (if (<0) -> intra prediction)
                           short l1_ref_idx)   // <-- reference frame for list1 prediction 
{
  int  i, j;
  int  block_x4  = block_x+4;
  int  block_y4  = block_y+4;
  imgpel* l0pred     = l0_pred;
  imgpel* l1pred     = l1_pred;
  short****** mv_array = img->all_mv;
  int max_imgpel_value_uv = img->max_imgpel_value_comp[1];
  imgpel (*curr_mpr)[16] = img->mpr[ uv + 1];

  int  apply_weights = ( (active_pps->weighted_pred_flag && (img->type == P_SLICE || img->type == SP_SLICE)) ||
                         (active_pps->weighted_bipred_idc && (img->type == B_SLICE)));

  if (currMB->bi_pred_me && l0_ref_idx == 0 && l1_ref_idx == 0 && p_dir == 2 && l0_mode==1 && l1_mode==1)
    mv_array = currMB->bi_pred_me == 1? img->bipred_mv1 : img->bipred_mv2 ;
  //===== INTRA PREDICTION =====
  if (p_dir==-1)
  {
    IntraChromaPrediction4x4 (currMB, uv, block_x, block_y);
    return;
  }

  //===== INTER PREDICTION =====
  if ((p_dir==0) || (p_dir==2))
  {
    (*OneComponentChromaPrediction4x4) (l0_pred, block_x, block_y, mv_array, LIST_0, l0_ref_idx, l0_mode, uv);
  }
  if ((p_dir==1) || (p_dir==2))
  {
    (*OneComponentChromaPrediction4x4) (l1_pred, block_x, block_y, mv_array, LIST_1, l1_ref_idx, l1_mode, uv);
  }
  if (apply_weights)
  {
    if (p_dir==2)
    {
      int wbp0 = wbp_weight[0][l0_ref_idx][l1_ref_idx][uv+1];
      int wbp1 = wbp_weight[1][l0_ref_idx][l1_ref_idx][uv+1];
      int offset = (wp_offset[0][l0_ref_idx][uv+1] + wp_offset[1][l1_ref_idx][uv+1] + 1)>>1;
      int wp_round = 2*wp_chroma_round;
      int weight_denom = luma_log_weight_denom + 1;


      for (j=block_y; j<block_y4; j++)
        for (i=block_x; i<block_x4; i++)
          curr_mpr[j][i] =  iClip1( max_imgpel_value_uv,
          ((wbp0 * *l0pred++ + wbp1 * *l1pred++ + wp_round) >> (weight_denom)) + (offset) );
    }
    else if (p_dir==0)
    {
      int wp = wp_weight[0][l0_ref_idx][uv+1];
      int offset = wp_offset[0][l0_ref_idx][uv+1];
      for (j=block_y; j<block_y4; j++)
        for (i=block_x; i<block_x4; i++)
          curr_mpr[j][i] = iClip1( max_imgpel_value_uv, (( wp * *l0pred++ + wp_chroma_round) >> chroma_log_weight_denom) +  offset);
    }
    else // (p_dir==1)
    {
      int wp = wp_weight[1][l1_ref_idx][uv+1];
      int offset = wp_offset[1][l1_ref_idx][uv+1];

      for (j=block_y; j<block_y4; j++)
        for (i=block_x; i<block_x4; i++)
          curr_mpr[j][i] = iClip1( max_imgpel_value_uv, ((wp * *l1pred++ + wp_chroma_round) >> chroma_log_weight_denom) + offset);
    }
  }
  else
  {
    if (p_dir==2)
    {
      for (j=block_y; j<block_y4; j++)
        for (i=block_x; i<block_x4; i++)
          curr_mpr[j][i] = (*l0pred++ + *l1pred++ + 1) >> 1;
    }
    else if (p_dir==0)
    {
      for (j=block_y; j<block_y4; j++)
      {
        memcpy(&(curr_mpr[j][block_x]), l0pred, BLOCK_SIZE * sizeof(imgpel));
        l0pred += BLOCK_SIZE;
      }
    }
    else // (p_dir==1)
    {
      for (j=block_y; j<block_y4; j++)
      {
        memcpy(&(curr_mpr[j][block_x]), l1pred, BLOCK_SIZE * sizeof(imgpel));
        l1pred += BLOCK_SIZE;
      }
    }
  }
}


/*!
 ************************************************************************
 * \brief
 *    Chroma residual coding for an macroblock
 ************************************************************************
 */
void ChromaResidualCoding (Macroblock *currMB)
{
  int chroma_cbp;
  static const int block8x8_idx[3][4][4] =     //ADD-VG-15052004
  {
    { {0, 1, 0, 0},
      {2, 3, 0, 0},
      {0, 0, 0, 0},
      {0, 0, 0, 0},
    },
    { {0, 1, 0, 0},
      {0, 1, 0, 0},
      {2, 3, 0, 0},
      {2, 3, 0, 0},
    },
    { {0, 0, 1, 1},
      {0, 0, 1, 1},
      {2, 2, 3, 3},
      {2, 2, 3, 3}
    }
  };
  int   uv, block8, block_y, block_x, j, i;
  int   l0_mode, l1_mode;
  short p_dir, refframe, l1_ref;
  int   skipped = (currMB->mb_type == 0 && (img->type == P_SLICE || img->type == SP_SLICE));
  int   yuv = img->yuv_format - 1; 
  imgpel (*curr_mpr)[16] = NULL;
  
  for (chroma_cbp = 0, uv=0; uv<2; uv++)
  {
    curr_mpr = img->mpr[ uv + 1];
    //===== prediction of chrominance blocks ===d==
    block8 = 0;
    for (block_y=0; block_y < img->mb_cr_size_y; block_y+=4)
    for (block_x=0; block_x < img->mb_cr_size_x; block_x+=4)
    {
      block8 = block8x8_idx[yuv][block_y>>2][block_x>>2];
      SetModesAndRefframe (currMB, block8, &p_dir, &l0_mode, &l1_mode, &refframe, &l1_ref);

      ChromaPrediction4x4 (currMB, uv, block_x, block_y, p_dir, l0_mode, l1_mode, refframe, l1_ref);
    }

    // ==== set chroma residue to zero for skip Mode in SP frames
    if (img->NoResidueDirect)
    {
      for (j=0; j<img->mb_cr_size_y; j++)
        memcpy(&enc_picture->imgUV[uv][img->pix_c_y+j][img->pix_c_x], curr_mpr[j], img->mb_cr_size_x * sizeof(imgpel));
    }
    else if (skipped && img->type==SP_SLICE)
    {
      for (j=0; j<img->mb_cr_size_y; j++)
        memset(img->m7[uv + 1][j], 0 , img->mb_cr_size_x * sizeof(int));
    }
    else if (skipped)
    {
      for (j=0; j<img->mb_cr_size_y; j++)
        memcpy(&enc_picture->imgUV[uv][img->pix_c_y+j][img->pix_c_x], curr_mpr[j], img->mb_cr_size_x * sizeof(imgpel));
    }
    else
    {
      for (j=0; j<img->mb_cr_size_y; j++)
        for (i=0; i<img->mb_cr_size_x; i++)
        {
          img->m7[uv + 1][j][i] = imgUV_org[uv][img->opix_c_y+j][img->opix_c_x+i] - curr_mpr[j][i];
        }
    }

    //===== DCT, Quantization, inverse Quantization, IDCT, and Reconstruction =====
    //===== Call function for skip mode in SP frames to properly process frame ====

    if (skipped && img->type==SP_SLICE)
    {
      if(si_frame_indicator || sp2_frame_indicator)
        chroma_cbp=dct_chroma_sp2(currMB, uv,chroma_cbp);
      else
        chroma_cbp=dct_chroma_sp(currMB, uv,chroma_cbp);
    }
    else
    {
      if (!img->NoResidueDirect && !skipped)
      {
        if (img->type!=SP_SLICE || (currMB->mb_type==I16MB ))
        {
          //even if the block is intra it should still be treated as SP
          chroma_cbp=dct_chroma   (currMB, uv,chroma_cbp);
        }
        else
        {
          if(si_frame_indicator||sp2_frame_indicator)
            chroma_cbp=dct_chroma_sp2(currMB, uv,chroma_cbp);// SI frames or switching SP frames
          else
            chroma_cbp=dct_chroma_sp(currMB, uv,chroma_cbp);
        }
      }
    }
  }

  //===== update currMB->cbp =====
  currMB->cbp += ((chroma_cbp)<<4);

}


/*!
 ************************************************************************
 * \brief
 *    Intra prediction of the chrminance layers of one macroblock
 ************************************************************************
 */
void IntraChromaPrediction (Macroblock *currMB, int *mb_up, int *mb_left, int*mb_up_left)
{
  int      s, s0, s1, s2, s3, i, j, k;
  imgpel**  image;
  int      block_x, block_y;
  int      mb_nr = img->current_mb_nr;
  int      mb_available_up;
  int      mb_available_left[2];
  int      mb_available_up_left;
  int      ih,iv;
  int      ib,ic,iaa;
  int      uv;
  imgpel   vline[16];
  int      mode;
  int      best_mode = DC_PRED_8;  //just an initilaization here, should always be overwritten
  int      cost;
  int      min_cost;
  PixelPos up;        //!< pixel position  p(0,-1)
  PixelPos left[17];  //!< pixel positions p(-1, -1..15)
  int      cr_MB_x = img->mb_cr_size_x;
  int      cr_MB_y = img->mb_cr_size_y;
  static imgpel (*cur_pred)[16];
  static imgpel (*curr_mpr_16x16)[16][16];
  static imgpel *hline;
  static imgpel *img_org, *img_prd;

  int      blk_x;
  int      blk_y;
  int      b8,b4;
  int      yuv = img->yuv_format - 1;
  int      dc_pred_value_chroma = img->dc_pred_value_comp[1];
  int      max_imgpel_value_uv  = img->max_imgpel_value_comp[1];

  static const int block_pos[3][4][4]= //[yuv][b8][b4]
  {
    { {0, 1, 2, 3},{0, 0, 0, 0},{0, 0, 0, 0},{0, 0, 0, 0}},
    { {0, 1, 2, 3},{2, 3, 2, 3},{0, 0, 0, 0},{0, 0, 0, 0}},
    { {0, 1, 2, 3},{1, 1, 3, 3},{2, 3, 2, 3},{3, 3, 3, 3}}
  };

  for (i=0;i<cr_MB_y+1;i++)
  {
    getNeighbour(mb_nr, -1 , i-1 , IS_CHROMA, &left[i]);
  }
  getNeighbour(mb_nr, 0 , -1 , IS_CHROMA, &up);


  mb_available_up                             = up.available;
  mb_available_up_left                        = left[0].available;
  mb_available_left[0] = mb_available_left[1] = left[1].available;

  if(input->UseConstrainedIntraPred)
  {
    mb_available_up = up.available ? img->intra_block[up.mb_addr] : 0;
    for (i=0, mb_available_left[0]=1; i<(cr_MB_y>>1);i++)
      mb_available_left[0]  &= left[i+1].available ? img->intra_block[left[i+1].mb_addr]: 0;
    for (i=(cr_MB_y>>1), mb_available_left[1]=1; i<cr_MB_y;i++)
      mb_available_left[1] &= left[i+1].available ? img->intra_block[left[i+1].mb_addr]: 0;
    mb_available_up_left = left[0].available ? img->intra_block[left[0].mb_addr]: 0;
  }

  if (mb_up)
    *mb_up = mb_available_up;
  if (mb_left)
    *mb_left = mb_available_left[0] && mb_available_left[1];
  if (mb_up_left)
    *mb_up_left = mb_available_up_left;


  // compute all chroma intra prediction modes for both U and V
  for (uv=0; uv<2; uv++)
  {
    image          = enc_picture->imgUV[uv];
    curr_mpr_16x16 = img->mpr_16x16[uv + 1];

    // DC prediction
    for(b8=0; b8<img->num_blk8x8_uv >> 1;b8++)
    {
      for (b4=0; b4<4; b4++)
      {
        block_y = subblk_offset_y[yuv][b8][b4];
        block_x = subblk_offset_x[yuv][b8][b4];
        blk_x = block_x;
        blk_y = block_y + 1;

        s=dc_pred_value_chroma;
        s0=s1=s2=s3=0;

        //===== get prediction value =====
        switch (block_pos[yuv][b8][b4])
        {
        case 0:  //===== TOP LEFT =====
          if      (mb_available_up)       for (i=blk_x;i<(blk_x+4);i++)  s0 += image[up.pos_y][up.pos_x + i];
          if      (mb_available_left[0])  for (i=blk_y;i<(blk_y+4);i++)  s2 += image[left[i].pos_y][left[i].pos_x];
          if      (mb_available_up && mb_available_left[0])  s  = (s0+s2+4) >> 3;
          else if (mb_available_up)                          s  = (s0   +2) >> 2;
          else if (mb_available_left[0])                     s  = (s2   +2) >> 2;
          break;
        case 1: //===== TOP RIGHT =====
          if      (mb_available_up)       for (i=blk_x;i<(blk_x+4);i++)  s1 += image[up.pos_y][up.pos_x + i];
          else if (mb_available_left[0])  for (i=blk_y;i<(blk_y+4);i++)  s2 += image[left[i].pos_y][left[i].pos_x];
          if      (mb_available_up)                          s  = (s1   +2) >> 2;
          else if (mb_available_left[0])                     s  = (s2   +2) >> 2;
          break;
        case 2: //===== BOTTOM LEFT =====
          if      (mb_available_left[1])  for (i=blk_y;i<(blk_y+4);i++)  s3 += image[left[i].pos_y][left[i].pos_x];
          else if (mb_available_up)       for (i=blk_x;i<(blk_x+4);i++)  s0 += image[up.pos_y][up.pos_x + i];
          if      (mb_available_left[1])                     s  = (s3   +2) >> 2;
          else if (mb_available_up)                          s  = (s0   +2) >> 2;
          break;
        case 3: //===== BOTTOM RIGHT =====
          if      (mb_available_up)       for (i=blk_x;i<(blk_x+4);i++)  s1 += image[up.pos_y][up.pos_x + i];
          if      (mb_available_left[1])  for (i=blk_y;i<(blk_y+4);i++)  s3 += image[left[i].pos_y][left[i].pos_x];
          if      (mb_available_up && mb_available_left[1])  s  = (s1+s3+4) >> 3;
          else if (mb_available_up)                          s  = (s1   +2) >> 2;
          else if (mb_available_left[1])                     s  = (s3   +2) >> 2;
          break;
        }

        //===== prediction =====
        cur_pred = curr_mpr_16x16[DC_PRED_8];
        for (j=block_y; j<block_y+4; j++)
          for (i=block_x; i<block_x+4; i++)
          {
            cur_pred[j][i] = s;
          }
      }
    }

    // vertical prediction    
    if (mb_available_up)
    {
      cur_pred = curr_mpr_16x16[VERT_PRED_8];
      //memcpy(hline,&image[up.pos_y][up.pos_x], cr_MB_x * sizeof(imgpel));
      hline = &image[up.pos_y][up.pos_x];
      for (j=0; j<cr_MB_y; j++)
        memcpy(cur_pred[j], hline, cr_MB_x * sizeof(imgpel));
    }

    // horizontal prediction
    if (mb_available_left[0] && mb_available_left[1])
    {
      cur_pred = curr_mpr_16x16[HOR_PRED_8];
      for (i=0; i<cr_MB_y; i++)
        vline[i] = image[left[i+1].pos_y][left[i+1].pos_x];
      for (j=0; j<cr_MB_y; j++)
      {
        int predictor = vline[j];
        for (i=0; i<cr_MB_x; i++)        
          cur_pred[j][i] = predictor;
      }
    }

    // plane prediction
    if (mb_available_left[0] && mb_available_left[1] && mb_available_up && mb_available_up_left)
    {
      ih = (cr_MB_x>>1)*(hline[cr_MB_x-1] - image[left[0].pos_y][left[0].pos_x]);
      for (i=0;i<(cr_MB_x>>1)-1;i++)
        ih += (i+1)*(hline[(cr_MB_x>>1)+i] - hline[(cr_MB_x>>1)-2-i]);

      iv = (cr_MB_y>>1)*(vline[cr_MB_y-1] - image[left[0].pos_y][left[0].pos_x]);
      for (i=0;i<(cr_MB_y>>1)-1;i++)
        iv += (i+1)*(vline[(cr_MB_y>>1)+i] - vline[(cr_MB_y>>1)-2-i]);

      ib= ((cr_MB_x == 8?17:5)*ih+2*cr_MB_x)>>(cr_MB_x == 8?5:6);
      ic= ((cr_MB_y == 8?17:5)*iv+2*cr_MB_y)>>(cr_MB_y == 8?5:6);

      iaa=16*(hline[cr_MB_x-1] + vline[cr_MB_y-1]);
      cur_pred = curr_mpr_16x16[PLANE_8];
      for (j=0; j<cr_MB_y; j++)
        for (i=0; i<cr_MB_x; i++)
          cur_pred[j][i]= iClip3(0, max_imgpel_value_uv,
           (iaa+(i-(cr_MB_x>>1)+1)*ib+(j-(cr_MB_y>>1)+1)*ic+16)>>5);
    }
  }

  if (!input->rdopt)      // the rd-opt part does not work correctly (see encode_one_macroblock)
  {                       // since ipredmodes could be overwritten => encoder-decoder-mismatches
    // pick lowest cost prediction mode
    min_cost = INT_MAX;
    for (i=0;i<cr_MB_y;i++)
    {
      getNeighbour(mb_nr, 0 , i, IS_CHROMA, &left[i]);
    }
    for (mode=DC_PRED_8; mode<=PLANE_8; mode++)
    {
      if ((img->type != I_SLICE || !input->IntraDisableInterOnly) && input->ChromaIntraDisable == 1 && mode!=DC_PRED_8)
        continue;

      if ((mode==VERT_PRED_8 && !mb_available_up) ||
        (mode==HOR_PRED_8 && (!mb_available_left[0] || !mb_available_left[1])) ||
        (mode==PLANE_8 && (!mb_available_left[0] || !mb_available_left[1] || !mb_available_up || !mb_available_up_left)))
        continue;

      cost = 0;
      for (uv=0; uv<2; uv++)
      {
        image = imgUV_org[uv];
        curr_mpr_16x16 = img->mpr_16x16[uv + 1];
        for (block_y=0; block_y<cr_MB_y; block_y+=4)
          for (block_x = 0; block_x < cr_MB_x; block_x += 4)
          {
            for (k=0, j = block_y; j < block_y + 4; j++)
            {
              img_prd = curr_mpr_16x16[mode][j];
              img_org = &image[left[j].pos_y][left[j].pos_x];
              for (i = block_x; i < block_x + 4; i++)
                diff[k++] = img_org[i] - img_prd[i];
            }
            cost += distortion4x4(diff);
          }
      }
      if (cost < min_cost)
      {
        best_mode = mode;
        min_cost = cost;
      }
    }
    currMB->c_ipred_mode = best_mode;
  }
}

/*!
 **************************************************************************************
 * \brief
 *    RD Decision for Intra prediction mode of the chrominance layers of one macroblock
 **************************************************************************************
 */
void IntraChromaRDDecision (Macroblock *currMB, RD_PARAMS enc_mb)
{
  int      i, j, k;
  imgpel** image;
  int      block_x, block_y;
  int      mb_nr = img->current_mb_nr;
  int      mb_available_up;
  int      mb_available_left[2];
  int      mb_available_up_left;
  int      uv;
  int      mode;
  int      best_mode = DC_PRED_8;  //just an initialization here, should always be overwritten
  int      cost;
  int      min_cost;
  PixelPos up;        //!< pixel position  p(0,-1)
  PixelPos left[17];  //!< pixel positions p(-1, -1..15)
  int      cr_MB_x = img->mb_cr_size_x;
  int      cr_MB_y = img->mb_cr_size_y;
  static imgpel *img_org, *img_prd;
  static imgpel (*curr_mpr_16x16)[16][16];

  for (i=0;i<cr_MB_y+1;i++)
  {
    getNeighbour(mb_nr, -1 , i-1 , IS_CHROMA, &left[i]);
  }
  getNeighbour(mb_nr, 0 , -1 , IS_CHROMA, &up);

  mb_available_up                             = up.available;
  mb_available_up_left                        = left[0].available;
  mb_available_left[0] = mb_available_left[1] = left[1].available;

  if(input->UseConstrainedIntraPred)
  {
    mb_available_up = up.available ? img->intra_block[up.mb_addr] : 0;
    for (i=1, mb_available_left[0] = 1; i < (cr_MB_y>>1) + 1; i++)
      mb_available_left[0]  &= left[i].available ? img->intra_block[left[i].mb_addr]: 0;

    for (i=(cr_MB_y>>1) + 1, mb_available_left[1]=1; i < cr_MB_y + 1;i++)
      mb_available_left[1] &= left[i].available ? img->intra_block[left[i].mb_addr]: 0;

    mb_available_up_left = left[0].available ? img->intra_block[left[0].mb_addr]: 0;
  }

  // pick lowest cost prediction mode
  min_cost = INT_MAX;
  for (i=0;i<cr_MB_y;i++)
  {
    getNeighbour(mb_nr, 0, i, IS_CHROMA, &left[i]);
  }
  if ( img->MbaffFrameFlag && img->field_mode )
  {
    for (i=0;i<cr_MB_y;i++)
    {
      left[i].pos_y = left[i].pos_y >> 1;
    }
  }

  for (mode=DC_PRED_8; mode<=PLANE_8; mode++)
  {
    if ((mode==VERT_PRED_8 && !mb_available_up) ||
      (mode==HOR_PRED_8 && (!mb_available_left[0] || !mb_available_left[1])) ||
      (mode==PLANE_8 && (!mb_available_left[0] || !mb_available_left[1] || !mb_available_up || !mb_available_up_left)))
      continue;

    cost = 0;
    for (uv=0; uv<2; uv++)
    {
      image = imgUV_org[uv];
      curr_mpr_16x16 = img->mpr_16x16[uv + 1];
      for (block_y=0; block_y<cr_MB_y; block_y+=4)
      {
        for (block_x=0; block_x<cr_MB_x; block_x+=4)
        {
          for (k=0,j=block_y; j<block_y+4; j++)
          {
            img_prd = curr_mpr_16x16[mode][j];
            img_org = &image[left[j].pos_y][left[j].pos_x];

            for (i = block_x; i < block_x + 4; i++)
              diff[k++] = img_org[i] - img_prd[i];
          }

          cost += distortion4x4(diff);
        }
        if (cost > min_cost) break;
      }
      if (cost > min_cost) break;
    }

    cost += (int) (enc_mb.lambda_me[Q_PEL] * mvbits[ mode ]); // exp golomb coding cost for mode signaling

    if (cost < min_cost)
    {
      best_mode = mode;
      min_cost = cost;
    }
  }
  currMB->c_ipred_mode = best_mode;
}


/*!
 ************************************************************************
 * \brief
 *    Check if all reference frames for a macroblock are zero
 ************************************************************************
 */
int ZeroRef (Macroblock* currMB)
{
  int i,j;

  for (j=img->block_y; j<img->block_y + BLOCK_MULTIPLE; j++)
  {
    for (i=img->block_x; i<img->block_x + BLOCK_MULTIPLE; i++)
    {
      if (enc_picture->ref_idx[LIST_0][j][i]!=0)
        return 0;
    }
  }
  return 1;
}


/*!
 ************************************************************************
 * \brief
 *    Converts macroblock type to coding value
 ************************************************************************
 */
int MBType2Value (Macroblock* currMB)
{
  static const int dir1offset[3]    =  { 1,  2, 3};
  static const int dir2offset[3][3] = {{ 0,  4,  8},   // 1. block forward
                                       { 6,  2, 10},   // 1. block backward
                                       {12, 14, 16}};  // 1. block bi-directional

  int mbtype, pdir0, pdir1;

  if (img->type!=B_SLICE)
  {
    if      (currMB->mb_type==I8MB ||currMB->mb_type==I4MB)
      return (img->type==I_SLICE ? 0 : 6);
    else if (currMB->mb_type==I16MB) return (img->type==I_SLICE ? 0 : 6) + img->i16offset;
    else if (currMB->mb_type==IPCM)  return (img->type==I_SLICE ? 25 : 31);
    else if (currMB->mb_type==P8x8)
    {
      if (input->symbol_mode==CAVLC
        && ZeroRef (currMB))         return 5;
      else                           return 4;
    }
    else                             return currMB->mb_type;
  }
  else
  {
    mbtype = currMB->mb_type;
    pdir0  = currMB->b8pdir[0];
    pdir1  = currMB->b8pdir[3];

    if      (mbtype==0)                    return 0;
    else if (mbtype==I4MB || mbtype==I8MB) return 23;
    else if (mbtype==I16MB)                return 23 + img->i16offset;
    else if (mbtype==IPCM)                 return 48;
    else if (mbtype==P8x8)                 return 22;
    else if (mbtype==1)                    return dir1offset[pdir0];
    else if (mbtype==2)                    return 4 + dir2offset[pdir0][pdir1];
    else                                   return 5 + dir2offset[pdir0][pdir1];
  }
}


/*!
************************************************************************
* \brief
*    Writes 4x4 intra prediction modes for a macroblock
************************************************************************
*/
int writeIntra4x4Modes(Macroblock *currMB)
{
  int i;
  SyntaxElement se;
  int           *bitCount   = currMB->bitcounter;
  Slice         *currSlice  = img->currentSlice;
  const int     *partMap    = assignSE2partition[input->partition_mode];
  DataPartition *dataPart   = &(currSlice->partArr[partMap[SE_INTRAPREDMODE]]);

  int rate = 0;

  currMB->IntraChromaPredModeFlag = 1;

  for(i=0;i<16;i++)
  {
    se.context = i;
    se.value1  = currMB->intra_pred_modes[i];
    se.value2  = 0;

#if TRACE
    if (se.value1 < 0 )
      snprintf(se.tracestring, TRACESTRING_SIZE, "Intra 4x4 mode  = predicted (context: %d)",se.context);
    else
      snprintf(se.tracestring, TRACESTRING_SIZE, "Intra 4x4 mode  = %3d (context: %d)",se.value1,se.context);
#endif

    // set symbol type and function pointers
    se.type = SE_INTRAPREDMODE;

    // encode and update rate
    writeIntraPredMode (&se, dataPart);

    bitCount[BITS_COEFF_Y_MB]+=se.len;
    rate += se.len;
  }

  return rate;
}

/*!
************************************************************************
* \brief
*    Writes 8x8 intra prediction modes for a macroblock
************************************************************************
*/
int writeIntra8x8Modes(Macroblock *currMB)
{
  int block8x8;
  SyntaxElement se;
  int           *bitCount   = currMB->bitcounter;
  Slice         *currSlice  = img->currentSlice;
  const int     *partMap    = assignSE2partition[input->partition_mode];
  DataPartition *dataPart   = &(currSlice->partArr[partMap[SE_INTRAPREDMODE]]);

  int rate = 0;

  currMB->IntraChromaPredModeFlag = 1;

  for(block8x8 = 0;block8x8 < 16; block8x8 += 4)
  {

    se.context = block8x8;
    se.value1  = currMB->intra_pred_modes8x8[block8x8];
    se.value2  = 0;

#if TRACE
    if (se.value1 < 0 )
      snprintf(se.tracestring, TRACESTRING_SIZE, "Intra 8x8 mode  = predicted (context: %d)",se.context);
    else
      snprintf(se.tracestring, TRACESTRING_SIZE, "Intra 8x8 mode  = %3d (context: %d)",se.value1,se.context);
#endif

    // set symbol type and function pointers
    se.type = SE_INTRAPREDMODE;

    // encode and update rate
    writeIntraPredMode (&se, dataPart);

    bitCount[BITS_COEFF_Y_MB]+=se.len;
    rate += se.len;
  }

  return rate;
}

int writeIntraModes(Macroblock *currMB)
{
  switch (currMB->mb_type)
  {
  case I4MB:
    return writeIntra4x4Modes(currMB);
    break;
  case I8MB:
    return writeIntra8x8Modes(currMB);
    break;
  default:
    return 0;
    break;
  }
}

/*!
 ************************************************************************
 * \brief
 *    Converts 8x8 block type to coding value
 ************************************************************************
 */
int B8Mode2Value (int b8mode, int b8pdir)
{
  static const int b8start[8] = {0,0,0,0, 1, 4, 5, 10};
  static const int b8inc  [8] = {0,0,0,0, 1, 2, 2, 1};

  return (img->type!=B_SLICE) ? (b8mode - 4) : b8start[b8mode] + b8inc[b8mode] * b8pdir;
}

/*!
************************************************************************
* \brief
*    Codes macroblock header
* \param rdopt
*    true for calls during RD-optimization
* \param coeff_rate
*    bitrate of Luma and Chroma coeff
************************************************************************
*/
int writeMBLayer (Macroblock *currMB, int rdopt, int *coeff_rate)
{
  int             i,j;
  int             mb_nr      = img->current_mb_nr;
  int             prev_mb_nr = FmoGetPreviousMBNr(img->current_mb_nr);
  Macroblock*     prevMB     = mb_nr ? (&img->mb_data[prev_mb_nr]) : NULL;
  SyntaxElement   se;
  int*            bitCount   = currMB->bitcounter;
  Slice*          currSlice  = img->currentSlice;
  DataPartition*  dataPart;
  const int*      partMap    = assignSE2partition[input->partition_mode];
  int             no_bits    = 0;
  int             skip       = currMB->mb_type ? 0:((img->type == B_SLICE) ? !currMB->cbp:1);
  int             mb_type;
  int             prevMbSkipped = 0;
  int             mb_field_tmp;
  Macroblock      *topMB = NULL;

  int             WriteFrameFieldMBInHeader = 0;

  if (img->MbaffFrameFlag)
  {
    if (0==(mb_nr & 0x01))
    {
      WriteFrameFieldMBInHeader = 1; // top field

      prevMbSkipped = 0;
    }
    else
    {
      if (prevMB->mb_type ? 0:((img->type == B_SLICE) ? !prevMB->cbp:1))
      {
        WriteFrameFieldMBInHeader = 1; // bottom, if top was skipped
      }

      topMB= &img->mb_data[prev_mb_nr];
      prevMbSkipped = topMB->skip_flag;
    }
  }
  currMB->IntraChromaPredModeFlag = IS_INTRA(currMB);

  // choose the appropriate data partition
  dataPart = &(currSlice->partArr[partMap[SE_MBTYPE]]);

  if(img->type == I_SLICE)
  {
    //========= write mb_aff (I_SLICE) =========
    if(WriteFrameFieldMBInHeader)
    {
      se.value1 = currMB->mb_field;
      se.value2 = 0;
      se.type   = SE_MBTYPE;

      TRACE_SE (se.tracestring, "mb_field_decoding_flag");
      writeFieldModeInfo(&se, dataPart);

      bitCount[BITS_MB_MODE] += se.len;
      no_bits                += se.len;
    }

    //========= write mb_type (I_SLICE) =========
    se.value1  = MBType2Value (currMB);
    se.value2  = 0;
    se.type    = SE_MBTYPE;

#if TRACE
    snprintf(se.tracestring, TRACESTRING_SIZE,   "mb_type (I_SLICE) (%2d,%2d) = %3d",img->mb_x, img->mb_y, currMB->mb_type);
#endif
    writeMB_typeInfo (&se, dataPart);

    bitCount[BITS_MB_MODE] += se.len;
    no_bits                += se.len;
  }
  // not I_SLICE, CABAC
  else if (input->symbol_mode == CABAC)
  {
    if (img->MbaffFrameFlag && ((img->current_mb_nr & 0x01) == 0||prevMbSkipped))
    {
      mb_field_tmp = currMB->mb_field;
      currMB->mb_field = field_flag_inference(currMB);
      CheckAvailabilityOfNeighborsCABAC(currMB);
      currMB->mb_field = mb_field_tmp;
    }

    //========= write mb_skip_flag (CABAC) =========
    mb_type    = MBType2Value (currMB);
    se.value1  = mb_type;
    se.value2  = currMB->cbp;
    se.type    = SE_MBTYPE;

    TRACE_SE (se.tracestring, "mb_skip_flag");
    writeMB_skip_flagInfo_CABAC(&se, dataPart);

    bitCount[BITS_MB_MODE] += se.len;
    no_bits                += se.len;

    CheckAvailabilityOfNeighborsCABAC(currMB);

    //========= write mb_aff (CABAC) =========
    if(img->MbaffFrameFlag && !skip) // check for copy mode
    {
      if(WriteFrameFieldMBInHeader)
      {
        se.value1 = currMB->mb_field;
        se.value2 = 0;
        se.type   =  SE_MBTYPE;

        TRACE_SE(se.tracestring, "mb_field_decoding_flag");
        writeFieldModeInfo(&se, dataPart);

        bitCount[BITS_MB_MODE] += se.len;
        no_bits                += se.len;
      }
    }

    //========= write mb_type (CABAC) =========
    if (currMB->mb_type != 0 || ((img->type == B_SLICE) && (currMB->cbp != 0 || cmp_cbp[0] !=0 || cmp_cbp[1]!=0 )))
    {
      se.value1  = mb_type;
      se.value2  = 0;
      se.type    = SE_MBTYPE;

#if TRACE
      if (img->type == B_SLICE)
        snprintf(se.tracestring, TRACESTRING_SIZE, "mb_type (B_SLICE) (%2d,%2d) = %3d",img->mb_x, img->mb_y, currMB->mb_type);
      else
        snprintf(se.tracestring, TRACESTRING_SIZE, "mb_type (P_SLICE) (%2d,%2d) = %3d",img->mb_x, img->mb_y, currMB->mb_type);
#endif
      writeMB_typeInfo( &se, dataPart);

      bitCount[BITS_MB_MODE] += se.len;
      no_bits                += se.len;
    }
  }
  // VLC not intra
  else if (currMB->mb_type != 0 || ((img->type == B_SLICE) && (currMB->cbp != 0 || cmp_cbp[0] != 0 || cmp_cbp[1]!=0 )))
  {
    //===== Run Length Coding: Non-Skipped macroblock =====
    se.value1  = img->cod_counter;
    se.value2  = 0;
    se.type    = SE_MBTYPE;

    TRACE_SE (se.tracestring, "mb_skip_run");
    writeSE_UVLC(&se, dataPart);

    bitCount[BITS_MB_MODE] += se.len;
    no_bits                += se.len;

    // Reset cod counter
    img->cod_counter = 0;

    // write mb_aff
    if(img->MbaffFrameFlag && !skip) // check for copy mode
    {
      if(WriteFrameFieldMBInHeader)
      {
        se.value1 = currMB->mb_field;
        se.type   =  SE_MBTYPE;

        TRACE_SE(se.tracestring, "mb_field_decoding_flag");
        writeSE_Flag (&se, dataPart);

        bitCount[BITS_MB_MODE] += se.len;
        no_bits                += se.len;
      }
    }
    // Put out mb mode
    se.value1  = MBType2Value (currMB);

    if (img->type != B_SLICE)
    {
      se.value1--;
    }
    se.type    = SE_MBTYPE;
    se.value2  = 0;

#if TRACE
    if (img->type == B_SLICE)
      snprintf(se.tracestring, TRACESTRING_SIZE, "mb_type (B_SLICE) (%2d,%2d) = %3d",img->mb_x, img->mb_y, currMB->mb_type);
    else
      snprintf(se.tracestring, TRACESTRING_SIZE, "mb_type (P_SLICE) (%2d,%2d) = %3d",img->mb_x, img->mb_y, currMB->mb_type);
#endif
    writeMB_typeInfo(&se, dataPart);

    bitCount[BITS_MB_MODE] += se.len;
    no_bits                += se.len;
  }
  else
  {
    //Run Length Coding: Skipped macroblock
    img->cod_counter++;

    currMB->skip_flag = 1;
    // CAVLC
    for (j=0; j < (4 + img->num_blk8x8_uv); j++)
      for (i=0; i < 4; i++)
        img->nz_coeff [img->current_mb_nr][i][j]=0;


    if(FmoGetNextMBNr(img->current_mb_nr) == -1 && img->cod_counter>0)
    {
      // Put out run
      se.value1  = img->cod_counter;
      se.value2  = 0;
      se.type    = SE_MBTYPE;

      TRACE_SE(se.tracestring, "mb_skip_run");
      writeSE_UVLC(&se, dataPart);

      bitCount[BITS_MB_MODE] += se.len;
      no_bits                += se.len;

      // Reset cod counter
      img->cod_counter = 0;
    }
  }

  //init NoMbPartLessThan8x8Flag
  currMB->NoMbPartLessThan8x8Flag = (IS_DIRECT(currMB) && !(active_sps->direct_8x8_inference_flag))? 0: 1;

  if (currMB->mb_type == IPCM)
  {
    dataPart = &(currSlice->partArr[partMap[SE_LUM_DC_INTRA]]);
    if (input->symbol_mode == CABAC)
    {
      int len;
      EncodingEnvironmentPtr eep = &dataPart->ee_cabac;
      len = arienco_bits_written(eep);
      arienco_done_encoding(eep); // This pads to byte
      len = arienco_bits_written(eep) - len;
      no_bits += len;
      // Now restart the encoder
      arienco_start_encoding(eep, dataPart->bitstream->streamBuffer, &(dataPart->bitstream->byte_pos));
      reset_pic_bin_count();
    }
    if (dataPart->bitstream->bits_to_go < 8)
    {
      // This will only happen in the CAVLC case, CABAC is already padded
      se.type  = SE_MBTYPE;
      se.len   = dataPart->bitstream->bits_to_go;
      no_bits += se.len;
      bitCount[BITS_COEFF_Y_MB]+= se.len;
      se.bitpattern = 0;
#if TRACE
      snprintf(se.tracestring, TRACESTRING_SIZE, "pcm_alignment_zero_bits = %d", se.len);
#endif
      writeSE_Fix(&se, dataPart->bitstream);
    }

    for (j = img->pix_y; j < img->pix_y + MB_BLOCK_SIZE; j++)
    {      
      for (i = img->pix_x; i < img->pix_x + MB_BLOCK_SIZE; i++)
      {
        se.len   = img->bitdepth_luma;
        se.type  = SE_MBTYPE;
        no_bits += se.len;
        se.bitpattern = enc_picture->imgY[j][i];
        se.value1 = se.bitpattern;
        bitCount[BITS_COEFF_Y_MB]+=se.len;
#if TRACE
        snprintf(se.tracestring, TRACESTRING_SIZE, "pcm_sample_luma (%d %d) = %d", j,i,se.bitpattern);
#endif
        writeSE_Fix(&se, dataPart->bitstream);
      }
    }

    if ((active_sps->chroma_format_idc != YUV400) && !IS_INDEPENDENT(input))
    {
      int uv;
      for (uv = 0; uv < 2; uv ++)
      {
        for (j = img->pix_c_y; j < img->pix_c_y + img->mb_cr_size_y; j++)
        {
          for (i = img->pix_c_x; i < img->pix_c_x + img->mb_cr_size_x; i++)
          {
            se.len   = img->bitdepth_chroma;
            se.type  = SE_MBTYPE;
            no_bits += se.len;
            se.bitpattern = enc_picture->imgUV[uv][j][i];
            se.value1 = se.bitpattern;
            bitCount[BITS_COEFF_UV_MB]+=se.len;
#if TRACE
            snprintf(se.tracestring, TRACESTRING_SIZE, "pcm_sample_chroma (%s) (%d %d) = %d", uv?"v":"u", j,i,se.bitpattern);
#endif
            writeSE_Fix(&se, dataPart->bitstream);
          }
        }
      }
    }
    return no_bits;
  }

  dataPart = &(currSlice->partArr[partMap[SE_MBTYPE]]);

  //===== BITS FOR 8x8 SUB-PARTITION MODES =====
  if (IS_P8x8 (currMB))
  {
    for (i=0; i<4; i++)
    {
      se.value1  = B8Mode2Value (currMB->b8mode[i], currMB->b8pdir[i]);
      se.value2  = 0;
      se.type    = SE_MBTYPE;
#if TRACE
      snprintf(se.tracestring, TRACESTRING_SIZE, "8x8 mode/pdir(%2d) = %3d/%d", i, currMB->b8mode[i], currMB->b8pdir[i]);
#endif
      writeB8_typeInfo (&se, dataPart);
      bitCount[BITS_MB_MODE]+= se.len;
      no_bits               += se.len;

      //set NoMbPartLessThan8x8Flag for P8x8 mode
      currMB->NoMbPartLessThan8x8Flag &= (currMB->b8mode[i]==0 && active_sps->direct_8x8_inference_flag) ||
        (currMB->b8mode[i]==4);
    }
    no_bits += writeMotionInfo2NAL  (currMB);
  }

  //============= Transform size flag for INTRA MBs =============
  //-------------------------------------------------------------
  //transform size flag for INTRA_4x4 and INTRA_8x8 modes
  if ((currMB->mb_type == I8MB || currMB->mb_type == I4MB) && input->Transform8x8Mode)
  {
    se.value1 = currMB->luma_transform_size_8x8_flag;
    se.type   = SE_MBTYPE;

#if TRACE
    snprintf(se.tracestring, TRACESTRING_SIZE, "transform_size_8x8_flag = %3d", currMB->luma_transform_size_8x8_flag);
#endif
    writeMB_transform_size(&se, dataPart);

    bitCount[BITS_MB_MODE] += se.len;
    no_bits                += se.len;
  }

  //===== BITS FOR INTRA PREDICTION MODES ====
  no_bits += writeIntraModes(currMB);
  //===== BITS FOR CHROMA INTRA PREDICTION MODE ====
  if (currMB->IntraChromaPredModeFlag && ((active_sps->chroma_format_idc != YUV400) && (active_sps->chroma_format_idc != YUV444)))
    no_bits += writeChromaIntraPredMode(currMB);
  else if(!rdopt) //GB CHROMA !!!!!
    currMB->c_ipred_mode = DC_PRED_8; //setting c_ipred_mode to default is not the right place here
                                      //resetting in rdopt.c (but where ??)
                                      //with cabac and bframes maybe it could crash without this default
                                      //since cabac needs the right neighborhood for the later MBs

  //----- motion information -----
  if (currMB->mb_type !=0 && currMB->mb_type !=P8x8)
  {
    no_bits  += writeMotionInfo2NAL  (currMB);
  }
  if ((currMB->mb_type!=0) || (img->type==B_SLICE && (currMB->cbp!=0 || cmp_cbp[0]!=0 || cmp_cbp[1]!=0)))
  {
    *coeff_rate = writeCBPandDquant (currMB);
    *coeff_rate += writeCoeff16x16 ( currMB, PLANE_Y, currMB->cbp );
    if ((active_sps->chroma_format_idc != YUV400) && (!IS_INDEPENDENT(input)))
    {
      if(active_sps->chroma_format_idc==YUV444)
      {
        *coeff_rate += writeCoeff16x16(currMB,PLANE_U, currMB->cbp);
        *coeff_rate += writeCoeff16x16(currMB,PLANE_V, currMB->cbp);
      }
      else
        *coeff_rate  += writeChromaCoeff (currMB);
    }
    no_bits  += *coeff_rate;
  }

  return no_bits;
}

void write_terminating_bit (short bit)
{
  DataPartition*          dataPart;
  const int*              partMap   = assignSE2partition[input->partition_mode];
  EncodingEnvironmentPtr  eep_dp;

  //--- write non-slice termination symbol if the macroblock is not the first one in its slice ---
  dataPart = &(img->currentSlice->partArr[partMap[SE_MBTYPE]]);
  dataPart->bitstream->write_flag = 1;
  eep_dp                          = &(dataPart->ee_cabac);

  biari_encode_symbol_final(eep_dp, bit);
#if TRACE
  fprintf (p_trace, "      CABAC terminating bit = %d\n",bit);
#endif

}


/*!
 ************************************************************************
 * \brief
 *    Write chroma intra prediction mode.
 ************************************************************************
 */
int writeChromaIntraPredMode(Macroblock* currMB)
{
  SyntaxElement   se;
  Slice*          currSlice = img->currentSlice;
  int*            bitCount  = currMB->bitcounter;
  const int*      partMap   = assignSE2partition[input->partition_mode];
  int             rate      = 0;
  DataPartition*  dataPart;

  //===== BITS FOR CHROMA INTRA PREDICTION MODES

  se.value1 = currMB->c_ipred_mode;
  se.value2 = 0;
  se.type = SE_INTRAPREDMODE;
  dataPart = &(currSlice->partArr[partMap[SE_INTRAPREDMODE]]);

  TRACE_SE(se.tracestring, "intra_chroma_pred_mode");
  writeCIPredMode(&se, dataPart);

  bitCount[BITS_COEFF_UV_MB] += se.len;
  rate                       += se.len;

  return rate;
}


/*!
 ************************************************************************
 * \brief
 *    Set global last_dquant according to macroblock delta qp
 ************************************************************************
 */

extern int last_dquant;

void set_last_dquant(Macroblock *currMB)
{
  if ((IS_INTERMV (currMB) || IS_INTRA (currMB))
    || ((img->type==B_SLICE)  && currMB->cbp != 0))
  {
    // non-skip
    last_dquant = currMB->delta_qp;
  }
  else
  {
    // skip
    last_dquant = 0;
  }
}


/*!
 ************************************************************************
 * \brief
 *    Passes the chosen syntax elements to the NAL
 ************************************************************************
 */
void write_one_macroblock (Macroblock* currMB, int eos_bit)
{
  int*        bitCount = currMB->bitcounter;
  int i;

  extern int cabac_encoding;

  // enable writing of trace file
#if TRACE
  Slice *curr_slice = img->currentSlice;
  curr_slice->partArr[0].bitstream->trace_enabled = TRUE;
  if (input->partition_mode)
  {
    curr_slice->partArr[1].bitstream->trace_enabled = TRUE;
    curr_slice->partArr[2].bitstream->trace_enabled = TRUE;
  }
#endif

  img->SumFrameQP += currMB->qp;

  //--- constrain intra prediction ---
  if(input->UseConstrainedIntraPred && (img->type==P_SLICE || img->type==B_SLICE))
  {
    img->intra_block[img->current_mb_nr] = IS_INTRA(currMB);
  }

  //===== init and update number of intra macroblocks =====
  if (img->current_mb_nr==0)
    intras=0;

  if (IS_INTRA(currMB))
    intras++;

  //--- write non-slice termination symbol if the macroblock is not the first one in its slice ---
  if (input->symbol_mode==CABAC && img->current_mb_nr!=img->currentSlice->start_mb_nr && eos_bit)
  {
    write_terminating_bit (0);
  }

#if TRACE
  // trace: write macroblock header
  if (p_trace)
  {
    fprintf(p_trace, "\n*********** Pic: %i (I/P) MB: %i Slice: %i **********\n\n", frame_no, img->current_mb_nr, img->current_slice_nr);
  }
#endif

  cabac_encoding = 1;

  //--- write macroblock ---
  writeMBLayer (currMB, 0, &i);  // i is temporary

  if (!((currMB->mb_type !=0 ) || ((img->type==B_SLICE) && currMB->cbp != 0) ))
  {
    for (i=0; i < 4; i++)
      memset(img->nz_coeff [img->current_mb_nr][i], 0, (4 + img->num_blk8x8_uv) * sizeof(int));  // CAVLC
  }

  set_last_dquant(currMB);

  //--- set total bit-counter ---
  if(img->yuv_format==YUV444 && !IS_INDEPENDENT(input))
    bitCount[BITS_TOTAL_MB] = bitCount[BITS_MB_MODE       ]  
                            + bitCount[BITS_COEFF_Y_MB    ]
                            + bitCount[BITS_INTER_MB      ] 
                            + bitCount[BITS_CBP_MB        ]
                            + bitCount[BITS_DELTA_QUANT_MB] 
                            + bitCount[BITS_COEFF_CB_MB   ] 
                            + bitCount[BITS_COEFF_CR_MB   ];
  else
    bitCount[BITS_TOTAL_MB] = bitCount[BITS_MB_MODE       ]  
                            + bitCount[BITS_COEFF_Y_MB    ]
                            + bitCount[BITS_INTER_MB      ] 
                            + bitCount[BITS_CBP_MB        ]
                            + bitCount[BITS_DELTA_QUANT_MB] 
                            + bitCount[BITS_COEFF_UV_MB   ];

  if ( input->RCEnable )
    rc_update_mb_stats( currMB, bitCount );  

  /*record the total number of MBs*/
  img->NumberofCodedMacroBlocks++;

  stats->bit_slice += bitCount[BITS_TOTAL_MB];

  cabac_encoding = 0;
}


/*!
 ************************************************************************
 * \brief
 *    Codes the reference frame
 ************************************************************************
 */
int writeReferenceFrame (Macroblock* currMB, int mode, int i, int j, int fwd_flag, int  ref)
{
  SyntaxElement   se;
  Slice*          currSlice = img->currentSlice;
  int*            bitCount  = currMB->bitcounter;
  const int*      partMap   = assignSE2partition[input->partition_mode];
  int             rate      = 0;
  DataPartition*  dataPart  = &(currSlice->partArr[partMap[SE_REFFRAME]]);
  int             list       = ( fwd_flag ? LIST_0 + currMB->list_offset: LIST_1 + currMB->list_offset);

  se.value1  = ref;
  se.type    = SE_REFFRAME;
  se.value2 = (fwd_flag)? LIST_0: LIST_1;

  img->subblock_x = i; // position used for context determination
  img->subblock_y = j; // position used for context determination

#if TRACE
  if (fwd_flag)
    snprintf(se.tracestring, TRACESTRING_SIZE, "ref_idx_l0 = %d", se.value1);
  else
    snprintf(se.tracestring, TRACESTRING_SIZE, "ref_idx_l1 = %d", se.value1);
#endif

  writeRefFrame[list](&se, dataPart);

  bitCount[BITS_INTER_MB] += se.len;
  rate                    += se.len;

  return rate;
}


/*!
 ************************************************************************
 * \brief
 *    Writes motion vectors of an 8x8 block
 ************************************************************************
 */
int writeMotionVector8x8 (Macroblock *currMB, 
                          int  i0,
                          int  j0,
                          int  i1,
                          int  j1,
                          int  refframe,
                          int  list_idx,
                          int  mv_mode)
{
  int            i, j, k, l, m;
  int            curr_mvd;
  DataPartition* dataPart;

  int            rate       = 0;
  int            step_h     = input->part_size[mv_mode][0];
  int            step_v     = input->part_size[mv_mode][1];
  SyntaxElement  se;
  Slice*         currSlice  = img->currentSlice;
  int*           bitCount   = currMB->bitcounter;
  const int*     partMap    = assignSE2partition[input->partition_mode];
  int            refindex   = refframe;

  short******    all_mv     = img->all_mv;
  short******    pred_mv    = img->pred_mv;  

  if (currMB->bi_pred_me && currMB->b8pdir[0]==2 && mv_mode == 1 && refindex == 0)
    all_mv = currMB->bi_pred_me == 1? img->bipred_mv1 : img->bipred_mv2 ;

  for (j = j0; j < j1; j += step_v)
  {
    img->subblock_y = j; // position used for context determination    
    for (i = i0; i < i1; i += step_h)
    {
      img->subblock_x = i; // position used for context determination      
      for (k=0; k<2; k++)
      {
        curr_mvd = all_mv[j][i][list_idx][refindex][mv_mode][k] - pred_mv[j][i][list_idx][refindex][mv_mode][k];

        //--- store (oversampled) mvd ---
        for (l = j; l < j + step_v; l++)
        {
          for (m = i; m < i + step_h; m++)
          {
            currMB->mvd[list_idx][l][m][k] = curr_mvd;
          }
        }
        dataPart = &(currSlice->partArr[partMap[SE_MVD]]);        
        se.value1 = curr_mvd;
        se.value2  = 2 * k + list_idx; // identifies the component and the direction; only used for context determination
        se.type   = SE_MVD;

#if TRACE
        if (!list_idx)
          snprintf(se.tracestring, TRACESTRING_SIZE, "mvd_l0 (%d) = %3d  (org_mv %3d pred_mv %3d)",k, curr_mvd, all_mv[j][i][list_idx][refindex][mv_mode][k], pred_mv[j][i][list_idx][refindex][mv_mode][k]);
        else
          snprintf(se.tracestring, TRACESTRING_SIZE, "mvd_l1 (%d) = %3d  (org_mv %3d pred_mv %3d)",k, curr_mvd, all_mv[j][i][list_idx][refindex][mv_mode][k], pred_mv[j][i][list_idx][refindex][mv_mode][k]);
#endif
        writeMVD (&se, dataPart);

        bitCount[BITS_INTER_MB] += se.len;
        rate                    += se.len;
      }
    }
  }
  return rate;
}


/*!
 ************************************************************************
 * \brief
 *    Writes motion info
 ************************************************************************
 */
int writeMotionInfo2NAL (Macroblock* currMB)
{
  int k, j0, i0;
  int             no_bits   = 0;
  int   bframe          = (img->type==B_SLICE);
  int   step_h0         = (input->blc_size[IS_P8x8(currMB) ? 4 : currMB->mb_type][0] >> 2);
  int   step_v0         = (input->blc_size[IS_P8x8(currMB) ? 4 : currMB->mb_type][1] >> 2);
  char  *cref_idx;

  //=== If multiple ref. frames, write reference frame for the MB ===
  if (IS_INTERMV (currMB))
  {
    // if CAVLC is turned on, a 8x8 macroblock with all ref=0 in a P-frame is signalled in macroblock mode
    if (!IS_P8x8 (currMB) || !ZeroRef (currMB) || input->symbol_mode==CABAC || bframe)
    {
      for (j0 = 0; j0 < 4; j0 += step_v0)
      {
        cref_idx = &enc_picture->ref_idx[LIST_0][img->block_y + j0][img->block_x];
        for (i0 = 0; i0 < 4; i0 += step_h0)
        {
          k = j0 + (i0 >> 1);
          if ((currMB->b8pdir[k]==0 || currMB->b8pdir[k]==2) && currMB->b8mode[k]!=0)//has forward vector
          {
            no_bits += writeReferenceFrame (currMB, currMB->b8mode[k], i0, j0, 1, cref_idx[i0]);
          }
        }
      }

      for (j0 = 0; j0 < 4; j0 += step_v0)
      {        
        cref_idx = &enc_picture->ref_idx[LIST_1][img->block_y + j0][img->block_x];
        for (i0 = 0; i0 < 4; i0 += step_h0)
        {
          k = j0 + (i0 >> 1);
          if ((currMB->b8pdir[k] == 1 || currMB->b8pdir[k] == 2) && currMB->b8mode[k]!=0)//has backward vector
          {
            no_bits += writeReferenceFrame (currMB, currMB->b8mode[k], i0, j0, 0, cref_idx[i0]);
          }
        }
      }
    }
  }

  //===== write forward motion vectors =====
  if (IS_INTERMV (currMB))
  {
    for (j0=0; j0<4; j0+=step_v0)
    {
      cref_idx = &enc_picture->ref_idx[LIST_0][img->block_y + j0][img->block_x];
      for (i0=0; i0<4; i0+=step_h0)
      {
        k=j0+(i0 >> 1);
        if ((currMB->b8pdir[k]==0 || currMB->b8pdir[k]==2) && currMB->b8mode[k]!=0)//has forward vector
        {
          no_bits  += writeMotionVector8x8 (currMB, i0, j0, i0 + step_h0, j0 + step_v0, cref_idx[i0], LIST_0, currMB->b8mode[k]);
        }
      }
    }
  }


  //===== write backward motion vectors =====
  if (IS_INTERMV (currMB) && bframe)
  {
    for (j0=0; j0<4; j0+=step_v0)
    {
      cref_idx = &enc_picture->ref_idx[LIST_1][img->block_y + j0][img->block_x];
      for (i0=0; i0<4; i0+=step_h0)
      {
        k=j0+(i0 >> 1);
        if ((currMB->b8pdir[k]==1 || currMB->b8pdir[k]==2) && currMB->b8mode[k]!=0)//has backward vector
        {          
          no_bits  += writeMotionVector8x8 (currMB, i0, j0, i0+step_h0, j0+step_v0, cref_idx[i0], LIST_1, currMB->b8mode[k]);
        }
      }
    }
  }
  return no_bits;
}



/*!
 ************************************************************************
 * \brief
 *    Writes chrominance coefficients
 ************************************************************************
 */
int writeChromaCoeff (Macroblock* currMB)
{
  int             rate      = 0;
  SyntaxElement   se;
  int*            bitCount  = currMB->bitcounter;
  Slice*          currSlice = img->currentSlice;
  const int*      partMap   = assignSE2partition[input->partition_mode];
  int             cbp       = currMB->cbp;
  DataPartition*  dataPart;

  int   level, run;
  int   k, uv;
  int   b8, b4, param;
  int*  ACLevel;
  int*  ACRun;
  int*  DCLevel;
  int*  DCRun;

  static const int   chroma_dc_context[3]={CHROMA_DC, CHROMA_DC_2x4, CHROMA_DC_4x4};
  int   yuv = img->yuv_format - 1;

  static const unsigned char chroma_ac_param[3][8][4] =
  {
   {{ 4, 20,  5, 21},
    {36, 52, 37, 53},
    { 0,  0,  0,  0},
    { 0,  0,  0,  0},
    { 0,  0,  0,  0},
    { 0,  0,  0,  0},
    { 0,  0,  0,  0},
    { 0,  0,  0,  0}},
   {{ 4, 20,  5, 21},
    { 6, 22,  7, 23},
    {36, 52, 37, 53},
    {38, 54, 39, 55},
    { 0,  0,  0,  0},
    { 0,  0,  0,  0},
    { 0,  0,  0,  0},
    { 0,  0,  0,  0}},
   {{ 4, 20,  5, 21},
    {36, 52, 37, 53},
    { 6, 22,  7, 23},
    {38, 54, 39, 55},
    { 8, 24,  9, 25},
    {40, 56, 41, 57},
    {10, 26, 11, 27},
    {42, 58, 43, 59}}
  };

  //=====
  //=====   D C - C O E F F I C I E N T S
  //=====
  if (cbp > 15)  // check if any chroma bits in coded block pattern is set
  {
    if (input->symbol_mode == CAVLC)
    {
      for (uv=0; uv < 2; uv++)
      {

        param = uv;
        rate += writeCoeff4x4_CAVLC (currMB, CHROMA_DC, 0, 0, param);
        // CAVLC
      }
    }
    else
    {
      for (uv=0; uv < 2; uv++)
      {

        DCLevel = img->cofDC[uv+1][0];
        DCRun   = img->cofDC[uv+1][1];

        level=1;
        for (k=0; k <= img->num_cdc_coeff && level != 0; ++k)
        {
          level = se.value1 = DCLevel[k]; // level
          run   = se.value2 = DCRun  [k]; // run

          se.context         = chroma_dc_context[yuv];
          se.type             = (IS_INTRA(currMB) ? SE_CHR_DC_INTRA : SE_CHR_DC_INTER);
          img->is_intra_block =  IS_INTRA(currMB);
          img->is_v_block     = uv;

          // choose the appropriate data partition
          dataPart = &(currSlice->partArr[partMap[se.type]]);
#if TRACE
          snprintf(se.tracestring, TRACESTRING_SIZE, "DC Chroma %2d: level =%3d run =%2d",k, level, run);
#endif
          writeRunLevel_CABAC(&se, dataPart);

          bitCount[BITS_COEFF_UV_MB] += se.len;
          rate                       += se.len;
        }
      }
    }
  }

  //=====
  //=====   A C - C O E F F I C I E N T S
  //=====
  uv=-1;
  if (cbp >> 4 == 2) // check if chroma bits in coded block pattern = 10b
  {
    if (input->symbol_mode == CAVLC)
    {
      for (b8=4; b8 < (4+img->num_blk8x8_uv); b8++)
      {
        for (b4=0; b4 < 4; b4++)
        {

          param = chroma_ac_param[yuv][b8-4][b4];
          rate += writeCoeff4x4_CAVLC (currMB, CHROMA_AC, b8, b4, param);
          // CAVLC
        }
      }
    }
    else
    {
      for (b8=4; b8 < (4+img->num_blk8x8_uv); b8++)
      {
        for (b4=0; b4 < 4; b4++)
        {

          ACLevel = img->cofAC[b8][b4][0];
          ACRun   = img->cofAC[b8][b4][1];

          level=1;
          uv++;

          img->subblock_y = subblk_offset_y[yuv][b8-4][b4]>>2;
          img->subblock_x = subblk_offset_x[yuv][b8-4][b4]>>2;

          for (k=0; k < 16 && level != 0; k++)
          {
            level = se.value1 = ACLevel[k]; // level
            run   = se.value2 = ACRun  [k]; // run

            se.context          = CHROMA_AC;
            se.type             = (IS_INTRA(currMB) ? SE_CHR_AC_INTRA : SE_CHR_AC_INTER);
            img->is_intra_block =  IS_INTRA(currMB);
            img->is_v_block     = (uv>=(img->num_blk8x8_uv<<1));

            // choose the appropriate data partition
            dataPart = &(currSlice->partArr[partMap[se.type]]);
#if TRACE
            snprintf(se.tracestring, TRACESTRING_SIZE, "AC Chroma %2d: level =%3d run =%2d",k, level, run);
#endif
            writeRunLevel_CABAC(&se, dataPart);
            bitCount[BITS_COEFF_UV_MB] += se.len;
            rate                       += se.len;
          }
        }
      }
    }
  }

  return rate;
}

/*!
 ************************************************************************
 * \brief
 *    Writes Coeffs of an 4x4 block
 ************************************************************************
 */
int writeCoeff4x4_CABAC (Macroblock* currMB, ColorPlane plane, int b8, int b4, int intra4x4mode)
{
  int             rate      = 0;
  SyntaxElement   se;
  Slice*          currSlice = img->currentSlice;
  const int*      partMap   = assignSE2partition[input->partition_mode];
  int*            bitCount  = currMB->bitcounter;
  DataPartition*  dataPart;

  int level, run;
  int k;
  int pl_off=plane<<2;
  int bits_coeff_mb=((plane==0)?(BITS_COEFF_Y_MB):((plane==1)?BITS_COEFF_CB_MB:BITS_COEFF_CR_MB));
  int*  ACLevel = img->cofAC[b8+pl_off][b4][0];
  int*  ACRun   = img->cofAC[b8+pl_off][b4][1];

  img->subblock_x = ((b8&0x1)==0) ? (((b4&0x1)==0)? 0: 1) : (((b4&0x1)==0)? 2: 3); // horiz. position for coeff_count context
  img->subblock_y = (b8<2)        ? ((b4<2)       ? 0: 1) : ((b4<2)       ? 2: 3); // vert.  position for coeff_count context

  level=1; // get inside loop
  for(k=0; k<=16 && level !=0; k++)
  {
    level = se.value1 = ACLevel[k]; // level
    run   = se.value2 = ACRun  [k]; // run

    se.context     = ((plane==0)?(LUMA_4x4):((plane==1)?CB_4x4:CR_4x4));
    se.type        = (k==0
      ? (intra4x4mode ? SE_LUM_DC_INTRA : SE_LUM_DC_INTER)
      : (intra4x4mode ? SE_LUM_AC_INTRA : SE_LUM_AC_INTER));
    img->is_intra_block = intra4x4mode;

    // choose the appropriate data partition
    dataPart = &(currSlice->partArr[partMap[se.type]]);
#if TRACE
    if (plane == 0)
    snprintf(se.tracestring, TRACESTRING_SIZE, "Luma sng(%2d) level =%3d run =%2d", k, level,run);
    else if (plane == 1)
    snprintf(se.tracestring, TRACESTRING_SIZE, "Cb   sng(%2d) level =%3d run =%2d", k, level,run);
    else
    snprintf(se.tracestring, TRACESTRING_SIZE, "Cr   sng(%2d) level =%3d run =%2d", k, level,run);        
#endif
    writeRunLevel_CABAC(&se, dataPart);
    bitCount[bits_coeff_mb] += se.len;
    rate                    += se.len;
  }
  return rate;
}

/*!
************************************************************************
* \brief
*    Writes coefficients of an 8x8 block (CABAC)
************************************************************************
*/
int writeCoeff8x8_CABAC (Macroblock* currMB, ColorPlane plane, int b8, int intra_mode)
{
  int             rate      = 0;
  SyntaxElement   se;
  Slice*          currSlice = img->currentSlice;
  const int*      partMap   = assignSE2partition[input->partition_mode];
  int*            bitCount  = currMB->bitcounter;
  DataPartition*  dataPart;

  int   level, run;
  int   k;

  int pl_off=plane<<2;
  int bits_coeff_mb=((plane==0)?(BITS_COEFF_Y_MB):((plane==1)?BITS_COEFF_CB_MB:BITS_COEFF_CR_MB));
  int*  ACLevel = img->cofAC[b8+pl_off][0][0];
  int*  ACRun   = img->cofAC[b8+pl_off][0][1];

  img->subblock_x = ((b8&0x1)==0)?0:2;  // horiz. position for coeff_count context
  img->subblock_y = (b8<2)?0:2;     // vert.  position for coeff_count context


  level=1; // get inside loop
  for(k=0; k<=64 && level !=0; k++)
  {
    level = se.value1 = ACLevel[k]; // level
    run   = se.value2 = ACRun  [k]; // run

    se.context     = ((plane==0)?(LUMA_8x8):((plane==1)?CB_8x8:CR_8x8));
    se.type        = (k==0
      ? (intra_mode ? SE_LUM_DC_INTRA : SE_LUM_DC_INTER)
      : (intra_mode ? SE_LUM_AC_INTRA : SE_LUM_AC_INTER));
    img->is_intra_block = intra_mode;

    // choose the appropriate data partition
    dataPart = &(currSlice->partArr[partMap[img->type != B_SLICE ? se.type : SE_BFRAME]]);

#if TRACE
    if (plane == 0)
    snprintf(se.tracestring, TRACESTRING_SIZE, "Luma8x8 sng(%2d) level =%3d run =%2d", k, level,run);
    else if (plane == 1)
    snprintf(se.tracestring, TRACESTRING_SIZE, "Cb8x8   sng(%2d) level =%3d run =%2d", k, level,run);
    else
    snprintf(se.tracestring, TRACESTRING_SIZE, "Cr8x8   sng(%2d) level =%3d run =%2d", k, level,run);        
#endif
    writeRunLevel_CABAC(&se, dataPart);
    bitCount[bits_coeff_mb] += se.len;
    rate                      += se.len;
  }
  return rate;
}

/*!
************************************************************************
* \brief
*    Writes Luma Coeff of an 8x8 block
************************************************************************
*/
int writeCoeff8x8 (Macroblock* currMB, ColorPlane pl, int block8x8, int block_mode, int transform_size_flag)
{
  int  block4x4, rate = 0;
  int intra4x4mode = (block_mode==IBLOCK);
  int block_color_type=((pl == 0) ? LUMA : ((pl == 1) ? CB : CR1));

  if (block_mode == I8MB)
    assert(transform_size_flag == 1);

  if((!transform_size_flag) || input->symbol_mode == CAVLC) // allow here if 4x4 or CAVLC
  {
    if (input->symbol_mode == CAVLC )
    {
      for (block4x4=0; block4x4<4; block4x4++)
        rate += writeCoeff4x4_CAVLC (currMB, block_color_type, block8x8, block4x4, (transform_size_flag)?(block_mode==I8MB):intra4x4mode);// CAVLC, pass new intra
    }
    else
    {
      for (block4x4=0; block4x4<4; block4x4++)
        rate += writeCoeff4x4_CABAC (currMB, pl, block8x8, block4x4, intra4x4mode);
    }
  }
  else
    rate += writeCoeff8x8_CABAC (currMB, pl, block8x8, (block_mode == I8MB));

  return rate;
}

/*!
 ************************************************************************
 * \brief
 *    Writes CBP, DQUANT of an macroblock
 ************************************************************************
 */
int writeCBPandDquant (Macroblock* currMB)
{

  int             rate      = 0;
  int*            bitCount  = currMB->bitcounter;
  SyntaxElement   se;
  Slice*          currSlice = img->currentSlice;
  const int*      partMap   = assignSE2partition[input->partition_mode];
  int             cbp       = currMB->cbp;
  DataPartition*  dataPart;
  int             need_transform_size_flag;   //ADD-VG-24062004

  if (!IS_NEWINTRA (currMB))
  {
    //=====   C B P   =====
    //---------------------
    se.value1 = cbp;
    se.type   = SE_CBP;

    // choose the appropriate data partition
    dataPart = &(currSlice->partArr[partMap[se.type]]);

#if TRACE
    snprintf(se.tracestring, TRACESTRING_SIZE, "CBP (%2d,%2d) = %3d",img->mb_x, img->mb_y, cbp);
#endif
    writeCBP (currMB, &se, dataPart);

    bitCount[BITS_CBP_MB] += se.len;
    rate                  += se.len;

    //============= Transform Size Flag for INTER MBs =============
    //-------------------------------------------------------------
    need_transform_size_flag = (((currMB->mb_type >= 1 && currMB->mb_type <= 3)||
                                (IS_DIRECT(currMB) && active_sps->direct_8x8_inference_flag) ||
                                (currMB->NoMbPartLessThan8x8Flag))
                                && currMB->mb_type != I8MB && currMB->mb_type != I4MB
                                && (currMB->cbp&15 || cmp_cbp[0]&15 || cmp_cbp[1]&15)
                                && input->Transform8x8Mode);

    if (need_transform_size_flag)
    {
      se.value1 = currMB->luma_transform_size_8x8_flag;
      se.type   = SE_MBTYPE;

#if TRACE
      snprintf(se.tracestring, TRACESTRING_SIZE, "transform_size_8x8_flag = %3d", currMB->luma_transform_size_8x8_flag);
#endif
      writeMB_transform_size(&se, dataPart);

      bitCount[BITS_MB_MODE] += se.len;
      rate                   += se.len;
    }
  }

  //=====   DQUANT   =====
  //----------------------
  if (cbp!=0 || IS_NEWINTRA (currMB))
  {
    se.value1 = currMB->delta_qp;
    se.type = SE_DELTA_QUANT;

    // choose the appropriate data partition
    dataPart = &(img->currentSlice->partArr[partMap[se.type]]);
#if TRACE
    snprintf(se.tracestring, TRACESTRING_SIZE, "Delta QP (%2d,%2d) = %3d",img->mb_x, img->mb_y, currMB->delta_qp);
#endif
    writeDquant (&se, dataPart);
    bitCount[BITS_DELTA_QUANT_MB] += se.len;
    rate                          += se.len;
  }

  return rate;
}

/*!
************************************************************************
* \brief
*    Write Luma Coeffcients, as well as Cb and Cr Coefficients in the 
*    case of 444 common mode, of an macroblock
************************************************************************
*/
int writeCoeff16x16 (Macroblock* currMB, ColorPlane plane, int cbp)
{
  int             mb_x, mb_y, i, j, k;
  int             level, run;
  int             rate      = 0;
  int*            bitCount  = currMB->bitcounter;
  SyntaxElement   se;
  Slice*          currSlice = img->currentSlice;
  const int*      partMap   = assignSE2partition[input->partition_mode];
  int   pl_off = plane<<2; 
  DataPartition*  dataPart;

  int   b8, b4;
  int*  DCLevel = img->cofDC[plane][0];
  int*  DCRun   = img->cofDC[plane][1];
  int*  ACLevel;
  int*  ACRun;
  int   data_type; 
  int   bits_coeff_mb=((plane==0)?(BITS_COEFF_Y_MB):((plane==1)?(BITS_COEFF_CB_MB):(BITS_COEFF_CR_MB)));

  if (img->yuv_format==YUV444 && !IS_INDEPENDENT(input))
  {
    for (i=0; i < 4; i++)
      for (k=4*plane; k<4*(plane+1); k++)
        img->nz_coeff [img->current_mb_nr][i][k] = 0;
  }
  else
  {
    for (i=0; i < 4; i++)
      memset(img->nz_coeff [img->current_mb_nr][i], 0, (4 + img->num_blk8x8_uv) * sizeof(int));
  }


  if (!IS_NEWINTRA (currMB))
  {
    //=====  L U M I N A N C E   =====
    //--------------------------------
    for (i=0; i<4; i++)  
    {
      if (cbp & (1<<i))
      {
        rate += writeCoeff8x8 (currMB, plane, i, currMB->b8mode[i], currMB->luma_transform_size_8x8_flag);
      }
    }
  }
  else
  {
    //=====  L U M I N A N C E   f o r   1 6 x 1 6   =====
    //----------------------------------------------------
    // DC coeffs
    if (input->symbol_mode == CAVLC)
    {
      switch (plane)
      {
      case 0:
      default:
        data_type = LUMA_INTRA16x16DC;
        break;
      case 1:
        data_type = CB_INTRA16x16DC;
        break;
      case 2:
        data_type = CR_INTRA16x16DC;
        break;            
      }
      rate += writeCoeff4x4_CAVLC (currMB, data_type, 0, 0, 0);  // CAVLC
    }
    else
    {
      level=1; // get inside loop
      img->is_intra_block = TRUE;
      switch (plane)
      {
      case 0:
      default:
        data_type = LUMA_16DC;
        break;
      case 1:
        data_type = CB_16DC;
        break;
      case 2:
        data_type = CR_16DC;
        break;            
      }

      for (k=0; k<=16 && level!=0; k++)
      {
        level = se.value1 = DCLevel[k]; // level
        run   = se.value2 = DCRun  [k]; // run   

        se.context = data_type;
        se.type    = SE_LUM_DC_INTRA;   // element is of type DC

        // choose the appropriate data partition
        dataPart = &(currSlice->partArr[partMap[se.type]]);

#if TRACE
        if (plane == 0)
          snprintf(se.tracestring, TRACESTRING_SIZE, "DC luma 16x16 sng(%2d) level =%3d run =%2d", k, level, run);
        else if (plane == 1)
          snprintf(se.tracestring, TRACESTRING_SIZE, "DC cb   16x16 sng(%2d) level =%3d run =%2d", k, level, run);
        else
          snprintf(se.tracestring, TRACESTRING_SIZE, "DC cr   16x16 sng(%2d) level =%3d run =%2d", k, level, run);
#endif
        writeRunLevel_CABAC(&se, dataPart);
        bitCount[bits_coeff_mb] += se.len;
        rate                    += se.len;
      }
    }

    // AC coeffs
    if (cbp & 15)
    {
      if (input->symbol_mode == CAVLC)
      {
        switch (plane)
        {
        case 0:
        default:
          data_type = LUMA_INTRA16x16AC;
          break;
        case 1:
          data_type = CB_INTRA16x16AC;
          break;
        case 2:
          data_type = CR_INTRA16x16AC;
          break;            
        }
        for (mb_y=0; mb_y < 4; mb_y += 2)
        {
          for (mb_x=0; mb_x < 4; mb_x += 2)
          {
            for (j=mb_y; j < mb_y+2; j++)
            {
              int j1 = 2*(j >> 1);
              int j2 = 2*(j & 0x01);
              for (i=mb_x; i < mb_x+2; i++)
              {
                rate += writeCoeff4x4_CAVLC (currMB, data_type, j1 + (i >> 1), j2 + (i & 0x01), 0);  // CAVLC
              }
            }
          }
        }
      }
      else
      {
        img->is_intra_block = TRUE;
        switch (plane)
        {
        case 0:
        default:
          data_type = LUMA_16AC;
          break;
        case 1:
          data_type = CB_16AC;
          break;
        case 2:
          data_type = CR_16AC;
          break;            
        }

        for (mb_y=0; mb_y < 4; mb_y += 2)
        {
          for (mb_x=0; mb_x < 4; mb_x += 2)
          {
            for (j=mb_y; j < mb_y+2; j++)
            {              
              int j1 = 2*(j >> 1);
              int j2 = 2*(j & 0x01);
              img->subblock_y = j;

              for (i=mb_x; i < mb_x+2; i++)
              {
                img->subblock_x = i;
                b8      = j1 + (i >> 1) + pl_off;
                b4      = j2 + (i & 0x01);

                ACLevel = img->cofAC[b8][b4][0];
                ACRun   = img->cofAC[b8][b4][1];
                              
                level=1; // get inside loop

                for (k=0; k < 16 && level !=0; k++)
                {
                  level = se.value1 = ACLevel[k]; // level
                  run   = se.value2 = ACRun  [k]; // run

                  se.context = data_type;
                  se.type    = SE_LUM_AC_INTRA;   // element is of type AC

                  // choose the appropriate data partition
                  dataPart = &(currSlice->partArr[partMap[se.type]]);
#if TRACE
                  if (plane == 0)
                    snprintf(se.tracestring, TRACESTRING_SIZE, "AC luma 16x16 sng(%2d) level =%3d run =%2d", k, level, run);
                  else if (plane == 1)
                    snprintf(se.tracestring, TRACESTRING_SIZE, "AC cb   16x16 sng(%2d) level =%3d run =%2d", k, level, run);
                  else
                    snprintf(se.tracestring, TRACESTRING_SIZE, "AC cr   16x16 sng(%2d) level =%3d run =%2d", k, level, run);
#endif
                  writeRunLevel_CABAC(&se, dataPart);
                  bitCount[bits_coeff_mb] += se.len;
                  rate                    += se.len;
                }
              }
            }
          }
        }
      }
    }
  }

  return rate;
}

/*!
 ************************************************************************
 * \brief
 *    Get the Prediction from the Neighboring Blocks for Number of Nonzero Coefficients
 *
 *    Luma Blocks
 ************************************************************************
 */
int predict_nnz(Macroblock *currMB, int block_type, int i,int j)
{
  PixelPos pix;

  int pred_nnz = 0;
  int cnt      = 0;
  int mb_nr    = img->current_mb_nr;

  // left block
  getLuma4x4Neighbour(mb_nr, (i << 2) - 1, (j << 2), &pix);

  if (IS_INTRA(currMB) && pix.available && active_pps->constrained_intra_pred_flag && ((input->partition_mode != 0) && !img->currentPicture->idr_flag))
  {
    pix.available &= img->intra_block[pix.mb_addr];
    if (!pix.available)
      cnt++;
  }

  if (pix.available)
  {
    switch (block_type)
    {
    case LUMA:
      pred_nnz += img->nz_coeff [pix.mb_addr ][pix.x][pix.y];
      cnt++;
      break;
    case CB:
      pred_nnz += img->nz_coeff [pix.mb_addr ][pix.x][4+pix.y];
      cnt++;
      break;
    case CR1:
      pred_nnz += img->nz_coeff [pix.mb_addr ][pix.x][8+pix.y];
      cnt++;
      break;
    default:
      error("writeCoeff4x4_CAVLC: Invalid block type", 600);
      break;
    }
  }

  // top block
  getLuma4x4Neighbour(mb_nr, (i<<2), (j<<2) - 1, &pix);

  if (IS_INTRA(currMB) && pix.available && active_pps->constrained_intra_pred_flag && ((input->partition_mode != 0) && !img->currentPicture->idr_flag))
  {
    pix.available &= img->intra_block[pix.mb_addr];
    if (!pix.available)
      cnt++;
  }

  if (pix.available)
  {
    switch (block_type)
    {
    case LUMA:
      pred_nnz += img->nz_coeff [pix.mb_addr ][pix.x][pix.y];
      cnt++;
      break;
    case CB:
      pred_nnz += img->nz_coeff [pix.mb_addr ][pix.x][4+pix.y];
      cnt++;
      break;
    case CR1:
      pred_nnz += img->nz_coeff [pix.mb_addr ][pix.x][8+pix.y];
      cnt++;
      break;
    default:
      error("writeCoeff4x4_CAVLC: Invalid block type", 600);
      break;
    }
  }

  if (cnt==2)
  {
    pred_nnz++;
    pred_nnz>>=1;
  }

  return pred_nnz;
}


/*!
 ************************************************************************
 * \brief
 *    Get the Prediction from the Neighboring Blocks for Number of Nonzero Coefficients
 *
 *    Chroma Blocks
 ************************************************************************
 */
int predict_nnz_chroma(Macroblock *currMB, int i,int j)
{
  PixelPos pix;

  int pred_nnz = 0;
  int cnt      = 0;
  int mb_nr    = img->current_mb_nr;

  if (img->yuv_format != YUV444)
  {
    //YUV420 and YUV422
    // left block
    getChroma4x4Neighbour(mb_nr, ((i & 0x01)<<2) - 1, ((j-4)<<2), &pix);

    if (IS_INTRA(currMB) && pix.available && active_pps->constrained_intra_pred_flag && ((input->partition_mode != 0) && !img->currentPicture->idr_flag))
    {
      pix.available &= img->intra_block[pix.mb_addr];
      if (!pix.available)
        cnt++;
    }

    if (pix.available)
    {
      pred_nnz = img->nz_coeff [pix.mb_addr ][2 * (i >> 1) + pix.x][4 + pix.y];
      cnt++;
    }

    // top block
    getChroma4x4Neighbour(mb_nr, ((i & 0x01)<<2), ((j-4)<<2) -1,  &pix);

    if (IS_INTRA(currMB) && pix.available && active_pps->constrained_intra_pred_flag && ((input->partition_mode != 0) && !img->currentPicture->idr_flag))
    {
      pix.available &= img->intra_block[pix.mb_addr];
      if (!pix.available)
        cnt++;
    }

    if (pix.available)
    {
      pred_nnz += img->nz_coeff [pix.mb_addr ][2 * (i >> 1) + pix.x][4 + pix.y];
      cnt++;
    }
  }


  if (cnt==2)
  {
    pred_nnz++;
    pred_nnz>>=1;
  }

  return pred_nnz;
}



/*!
 ************************************************************************
 * \brief
 *    Writes coeff of an 4x4 block (CAVLC)
 *
 * \author
 *    Karl Lillevold <karll@real.com>
 *    contributions by James Au <james@ubvideo.com>
 ************************************************************************
 */
int writeCoeff4x4_CAVLC (Macroblock* currMB, int block_type, int b8, int b4, int param)
{
  int           no_bits    = 0;
  SyntaxElement se;
  int           *bitCount  = currMB->bitcounter;
  Slice         *currSlice = img->currentSlice;
  DataPartition *dataPart;
  int           *partMap   = assignSE2partition[input->partition_mode];

  int k,level = 1,run,vlcnum;
  int numcoeff = 0, lastcoeff = 0, numtrailingones = 0; 
  int numones = 0, totzeros = 0, zerosleft, numcoef;
  int numcoeff_vlc;
  int code, level_two_or_higher;
  int dptype = 0, bitcounttype = 0;
  int nnz, max_coeff_num = 0, cdc = 0, cac = 0;
  int subblock_x, subblock_y;
#if TRACE
  char type[15];
#endif

  static const int incVlc[] = {0, 3, 6, 12, 24, 48, 32768};  // maximum vlc = 6


  int*  pLevel = NULL;
  int*  pRun = NULL;

  switch (block_type)
  {
  case LUMA:
    max_coeff_num = 16;
    bitcounttype = BITS_COEFF_Y_MB;

    pLevel = img->cofAC[b8][b4][0];
    pRun   = img->cofAC[b8][b4][1];
#if TRACE
    sprintf(type, "%s", "Luma");
#endif
    dptype = (IS_INTRA (currMB)) ? SE_LUM_AC_INTRA : SE_LUM_AC_INTER;
    break;
  case LUMA_INTRA16x16DC:
    max_coeff_num = 16;
    bitcounttype = BITS_COEFF_Y_MB;

    pLevel = img->cofDC[0][0];
    pRun   = img->cofDC[0][1];
#if TRACE
    sprintf(type, "%s", "Lum16DC");
#endif
    dptype = SE_LUM_DC_INTRA;
    break;
  case LUMA_INTRA16x16AC:
    max_coeff_num = 15;
    bitcounttype = BITS_COEFF_Y_MB;

    pLevel = img->cofAC[b8][b4][0];
    pRun   = img->cofAC[b8][b4][1];
#if TRACE
    sprintf(type, "%s", "Lum16AC");
#endif
    dptype = SE_LUM_AC_INTRA;
    break;
  case CB:
    max_coeff_num = 16;
    bitcounttype = BITS_COEFF_CB_MB;

    pLevel = img->cofAC[4+b8][b4][0];
    pRun   = img->cofAC[4+b8][b4][1];
#if TRACE    
    sprintf(type, "%s", "CB");
#endif
    dptype = (IS_INTRA (currMB)) ? SE_LUM_AC_INTRA : SE_LUM_AC_INTER;
    break;
  case CB_INTRA16x16DC:
    max_coeff_num = 16;
    bitcounttype = BITS_COEFF_CB_MB;

    pLevel = img->cofDC[1][0];
    pRun   = img->cofDC[1][1];
#if TRACE    
    sprintf(type, "%s", "CB_16DC");
#endif
    dptype = SE_LUM_DC_INTRA;
    break;
  case CB_INTRA16x16AC:
    max_coeff_num = 15;
    bitcounttype = BITS_COEFF_CB_MB;

    pLevel = img->cofAC[4+b8][b4][0];
    pRun   = img->cofAC[4+b8][b4][1];
#if TRACE    
    sprintf(type, "%s", "CB_16AC");
#endif
    dptype = SE_LUM_AC_INTRA;
    break;

  case CR1:
    max_coeff_num = 16;
    bitcounttype = BITS_COEFF_CR_MB;

    pLevel = img->cofAC[8+b8][b4][0];
    pRun   = img->cofAC[8+b8][b4][1];
#if TRACE    
    sprintf(type, "%s", "CR");
#endif
    dptype = (IS_INTRA (currMB)) ? SE_LUM_AC_INTRA : SE_LUM_AC_INTER;
    break;
  case CR_INTRA16x16DC:
    max_coeff_num = 16;
    bitcounttype = BITS_COEFF_CR_MB;

    pLevel = img->cofDC[2][0];
    pRun   = img->cofDC[2][1];
#if TRACE    
    sprintf(type, "%s", "CR_16DC");
#endif
    dptype = SE_LUM_DC_INTRA;
    break;
  case CR_INTRA16x16AC:
    max_coeff_num = 15;
    bitcounttype = BITS_COEFF_CR_MB;

    pLevel = img->cofAC[8+b8][b4][0];
    pRun   = img->cofAC[8+b8][b4][1];
#if TRACE    
    sprintf(type, "%s", "CR_16AC");
#endif
    dptype = SE_LUM_AC_INTRA;
    break;

  case CHROMA_DC:
    max_coeff_num = img->num_cdc_coeff;
    bitcounttype = BITS_COEFF_UV_MB;
    cdc = 1;

    pLevel = img->cofDC[param+1][0];
    pRun   = img->cofDC[param+1][1];
#if TRACE
    sprintf(type, "%s", "ChrDC");
#endif
    dptype = (IS_INTRA (currMB)) ? SE_CHR_DC_INTRA : SE_CHR_DC_INTER;
    break;
  case CHROMA_AC:
    max_coeff_num = 15;
    bitcounttype = BITS_COEFF_UV_MB;
    cac = 1;

    pLevel = img->cofAC[b8][b4][0];
    pRun   = img->cofAC[b8][b4][1];
#if TRACE
    sprintf(type, "%s", "ChrAC");
#endif
    dptype = (IS_INTRA (currMB)) ? SE_CHR_AC_INTRA : SE_CHR_AC_INTER;
    break;
  default:
    error("writeCoeff4x4_CAVLC: Invalid block type", 600);
    break;
  }

  dataPart = &(currSlice->partArr[partMap[dptype]]);

  for(k = 0; (k <= ((cdc) ? img->num_cdc_coeff : 16)) && level != 0; k++)
  {
    level = pLevel[k]; // level
    run   = pRun[k];   // run

    if (level)
    {

      totzeros += run; // lets add run always (even if zero) to avoid conditional
      if (iabs(level) == 1)
      {
        numones ++;
        numtrailingones ++;
        numtrailingones = imin(numtrailingones, 3); // clip to 3
      }
      else
      {
        numtrailingones = 0;
      }
      numcoeff ++;
      lastcoeff = k;
    }
  }

  if (!cdc)
  {
    if(block_type==LUMA || block_type==LUMA_INTRA16x16DC || block_type==LUMA_INTRA16x16AC
      ||block_type==CHROMA_AC)
    {
      if (!cac)
      {
        // luma
        subblock_x = ((b8 & 0x1) == 0) ? (((b4 & 0x1) == 0) ? 0 : 1) : (((b4 & 0x1) == 0) ? 2 : 3);
        // horiz. position for coeff_count context
        subblock_y = (b8 < 2) ? ((b4 < 2) ? 0 : 1) : ((b4 < 2) ? 2 : 3);
        // vert.  position for coeff_count context
        nnz = predict_nnz(currMB, LUMA, subblock_x,subblock_y);
      }
      else
      {
        // chroma AC
        subblock_x = param >> 4;
        subblock_y = param & 15;
        nnz = predict_nnz_chroma(currMB, subblock_x, subblock_y);
      }
      img->nz_coeff [img->current_mb_nr ][subblock_x][subblock_y] = numcoeff;
    }
    else if (block_type==CB || block_type==CB_INTRA16x16DC 
      || block_type==CB_INTRA16x16AC)
    {   //CB in the common mode in 4:4:4 profiles
      subblock_x = ((b8 & 0x1) == 0)?(((b4 & 0x1) == 0) ? 0 : 1):(((b4 & 0x1) == 0) ? 2 : 3); 
      // horiz. position for coeff_count context
      subblock_y = (b8 < 2) ? ((b4 < 2) ? 0 : 1) : ((b4 < 2) ? 2 : 3); 
      // vert.  position for coeff_count context
      nnz = predict_nnz(currMB, CB, subblock_x,subblock_y);
      img->nz_coeff [img->current_mb_nr ][subblock_x][4+subblock_y] = numcoeff;
    }
    else
    { //CR in the common mode in 4:4:4 profiles 
      subblock_x = ((b8 & 0x1) == 0)?(((b4 & 0x1) == 0) ? 0 : 1) : (((b4 & 0x1) == 0) ? 2 : 3); 
      // horiz. position for coeff_count context
      subblock_y = (b8 < 2) ? ((b4 < 2) ? 0 : 1) : ((b4 < 2) ? 2 : 3); 
      // vert.  position for coeff_count context
      nnz = predict_nnz(currMB, CR1, subblock_x,subblock_y);
      img->nz_coeff [img->current_mb_nr ][subblock_x][8+subblock_y] = numcoeff;
    }

    if (nnz < 2)
    {
      numcoeff_vlc = 0;
    }
    else if (nnz < 4)
    {
      numcoeff_vlc = 1;
    }
    else if (nnz < 8)
    {
      numcoeff_vlc = 2;
    }
    else
    {
      numcoeff_vlc = 3;
    }

  }
  else
  {
    // chroma DC (has its own VLC)
    // numcoeff_vlc not relevant
    numcoeff_vlc = 0;

    subblock_x = param;
    subblock_y = param;
  }

  se.type  = dptype;

  se.value1 = numcoeff;
  se.value2 = numtrailingones;
  se.len    = numcoeff_vlc; /* use len to pass vlcnum */

#if TRACE
  snprintf(se.tracestring,
    TRACESTRING_SIZE, "%s # c & tr.1s(%d,%d) vlc=%d #c=%d #t1=%d",
    type, subblock_x, subblock_y, numcoeff_vlc, numcoeff, numtrailingones);
#endif

  if (!cdc)
    writeSyntaxElement_NumCoeffTrailingOnes(&se, dataPart);
  else
    writeSyntaxElement_NumCoeffTrailingOnesChromaDC(&se, dataPart);

  bitCount[bitcounttype] += se.len;
  no_bits                += se.len;

  if (!numcoeff)
    return no_bits;

  if (numcoeff)
  {
    code = 0;
    for (k = lastcoeff; k > lastcoeff - numtrailingones; k--)
    {
      level = pLevel[k]; // level
      
      if (iabs(level) > 1)
      {
        printf("ERROR: level > 1\n");
        exit(-1);
      }
      code <<= 1;
      if (level < 0)
      {
        code |= 0x1;
      }
    }

    if (numtrailingones)
    {
      se.type  = dptype;

      se.value2 = numtrailingones;
      se.value1 = code;

#if TRACE
      snprintf(se.tracestring,
        TRACESTRING_SIZE, "%s trailing ones sign (%d,%d)",
        type, subblock_x, subblock_y);
#endif

      writeSyntaxElement_VLC (&se, dataPart);
      bitCount[bitcounttype] += se.len;
      no_bits                += se.len;

    }

    // encode levels
    level_two_or_higher = (numcoeff > 3 && numtrailingones == 3) ? 0 : 1;

    vlcnum = (numcoeff > 10 && numtrailingones < 3) ? 1 : 0;

    for (k = lastcoeff - numtrailingones; k >= 0; k--)
    {
      level = pLevel[k]; // level

      se.value1 = level;
      se.type  = dptype;

#if TRACE
      snprintf(se.tracestring,
        TRACESTRING_SIZE, "%s lev (%d,%d) k=%d vlc=%d lev=%3d",
        type, subblock_x, subblock_y, k, vlcnum, level);
#endif

      if (level_two_or_higher)
      {
        level_two_or_higher = 0;
        if (se.value1 > 0)
          se.value1 --;
        else
          se.value1 ++;        
      }

      //    encode level
      if (vlcnum == 0)
        writeSyntaxElement_Level_VLC1(&se, dataPart, active_sps->profile_idc);
      else
        writeSyntaxElement_Level_VLCN(&se, vlcnum, dataPart, active_sps->profile_idc);

      // update VLC table
      if (iabs(level) > incVlc[vlcnum])
        vlcnum++;

      if ((k == lastcoeff - numtrailingones) && iabs(level) > 3)
        vlcnum = 2;

      bitCount[bitcounttype] += se.len;
      no_bits                += se.len;
    }

    // encode total zeroes
    if (numcoeff < max_coeff_num)
    {

      se.type  = dptype;
      se.value1 = totzeros;

      vlcnum = numcoeff - 1;

      se.len = vlcnum;

#if TRACE
      snprintf(se.tracestring,
        TRACESTRING_SIZE, "%s totalrun (%d,%d) vlc=%d totzeros=%3d",
        type, subblock_x, subblock_y, vlcnum, totzeros);
#endif
      if (!cdc)
        writeSyntaxElement_TotalZeros(&se, dataPart);
      else
        writeSyntaxElement_TotalZerosChromaDC(&se, dataPart);

      bitCount[bitcounttype] += se.len;
      no_bits                += se.len;
    }

    // encode run before each coefficient
    zerosleft = totzeros;
    numcoef = numcoeff;
    for (k = lastcoeff; k >= 0; k--)
    {
      run = pRun[k]; // run

      se.value1 = run;
      se.type   = dptype;

      // for last coeff, run is remaining totzeros
      // when zerosleft is zero, remaining coeffs have 0 run
      if ((!zerosleft) || (numcoeff <= 1 ))
        break;

      if (numcoef > 1 && zerosleft)
      {
        vlcnum = imin(zerosleft - 1, RUNBEFORE_NUM_M1);
        se.len = vlcnum;

#if TRACE
        snprintf(se.tracestring,
          TRACESTRING_SIZE, "%s run (%d,%d) k=%d vlc=%d run=%2d",
          type, subblock_x, subblock_y, k, vlcnum, run);
#endif

        writeSyntaxElement_Run(&se, dataPart);

        bitCount[bitcounttype]+=se.len;
        no_bits               +=se.len;

        zerosleft -= run;
        numcoef --;
      }
    }
  }

  return no_bits;
}

int distortion_hadamard(imgpel **img_org, imgpel pred_img[16][16])
{
  imgpel *cur_img, *prd_img;
  int current_intra_sad_2 = 0;
  int ii, jj, i, j;
  static int M0[4][4][4][4], M4[4][4];
  static int (*M7)[4];

  for (j=0;j<16;j++)
  {
    cur_img = &img_org[img->opix_y + j][img->opix_x];
    prd_img = pred_img[j];
    for (i=0;i<16;i++)
    {
      M0[j >> 2][i >> 2][j & 0x03][i & 0x03] = cur_img[i] - prd_img[i];
    }
  }

  for (jj=0;jj<4;jj++)
  {
    for (ii=0;ii<4;ii++)
    {
      M7 = M0[jj][ii];
      hadamard4x4(M7, M7);
      for (j=0;j<4;j++)
      {
        for (i=0;i<4;i++)
        {
          if((i + j) != 0)
            current_intra_sad_2 += iabs(M7[j][i]);
        }
      }
    }
  }

  for (j=0;j<4;j++)
  {
    for (i=0;i<4;i++)
      M4[j][i] = (M0[j][i][0][0] >> 1);
  }
  // Hadamard of DC coeff
  hadamard4x4(M4, M4);
  for (j=0;j<4;j++)
  {
    for (i=0;i<4;i++)
    {
      current_intra_sad_2 += iabs(M4[j][i]);
    }
  }

  return current_intra_sad_2;
}

int distortion_sad(imgpel **img_org, imgpel pred_img[16][16])
{
  imgpel *cur_img, *prd_img;
  int current_intra_sad_2 = 0;
  int i, j; 

  for (j=0;j<16;j++)
  {
    cur_img = &img_org[img->opix_y + j][img->opix_x];
    prd_img = pred_img[j];
    for (i=0;i<16;i++)
    {
      current_intra_sad_2 += iabs( *cur_img++ - *prd_img++ );
    }
  }
  return current_intra_sad_2;
}

int distortion_sse(imgpel **img_org, imgpel pred_img[16][16])
{
  imgpel *cur_img, *prd_img;
  int current_intra_sad_2 = 0;
  int i, j; 

  for (j=0;j<16;j++)
  {
    cur_img = &img_org[img->opix_y + j][img->opix_x];
    prd_img = pred_img[j];
    for (i=0;i<16;i++)
    {
      current_intra_sad_2 += img->quad[ *cur_img++ - *prd_img++ ];
    }
  }
  return current_intra_sad_2;
}

/*!
 ************************************************************************
 * \brief
 *    Find best 16x16 based intra mode
 *
 * \par Input:
 *    Image parameters, pointer to best 16x16 intra mode
 *
 * \par Output:
 *    best 16x16 based SAD
 ************************************************************************/
double find_sad_16x16(int *intra_mode)
{
  double current_intra_sad_2, best_intra_sad2 = 1e30;

  int i,k;
  int mb_nr = img->current_mb_nr;
  imgpel  (*curr_mpr_16x16)[16][16] = img->mpr_16x16[0];

  PixelPos up;          //!< pixel position p(0,-1)
  PixelPos left[17];    //!< pixel positions p(-1, -1..15)

  int up_avail, left_avail, left_up_avail;

  for (i=0;i<17;i++)
  {
    getNeighbour(mb_nr, -1 ,  i - 1 , IS_LUMA, &left[i]);
  }

  getNeighbour(mb_nr, 0     ,  -1 , IS_LUMA, &up);

  if (!(input->UseConstrainedIntraPred))
  {
    up_avail   = up.available;
    left_avail = left[1].available;
    left_up_avail = left[0].available;
  }
  else
  {
    up_avail      = up.available ? img->intra_block[up.mb_addr] : 0;
    for (i = 1, left_avail = 1; i < 17;i++)
      left_avail  &= left[i].available ? img->intra_block[left[i].mb_addr]: 0;
    left_up_avail = left[0].available ? img->intra_block[left[0].mb_addr]: 0;
  }

  *intra_mode = DC_PRED_16;

  for (k = 0;k < 4; k++)
  {
    if (input->IntraDisableInterOnly == 0 || img->type != I_SLICE)
    {
      if (input->Intra16x16ParDisable && (k==VERT_PRED_16||k==HOR_PRED_16))
        continue;

      if (input->Intra16x16PlaneDisable && k==PLANE_16)
        continue;
    }
    //check if there are neighbours to predict from
    if (!((k==0 && !up_avail) || (k==1 && !left_avail) || (k==3 && (!left_avail || !up_avail || !left_up_avail))))
    {
      switch(input->ModeDecisionMetric)
      {
      case ERROR_SAD:
        current_intra_sad_2 = distortion_sad(pCurImg, curr_mpr_16x16[k]);
        if (img->yuv_format==YUV444 && !IS_INDEPENDENT(input))
        {
          current_intra_sad_2 += distortion_sad(imgUV_org[0], img->mpr_16x16[1][k]);
          current_intra_sad_2 += distortion_sad(imgUV_org[1], img->mpr_16x16[2][k]);
        }
        break;
      case ERROR_SSE:
        current_intra_sad_2 = distortion_sse(pCurImg, curr_mpr_16x16[k]);
        if (img->yuv_format==YUV444 && !IS_INDEPENDENT(input))
        {
          current_intra_sad_2 += distortion_sse(imgUV_org[0], img->mpr_16x16[1][k]);
          current_intra_sad_2 += distortion_sse(imgUV_org[1], img->mpr_16x16[2][k]);
        }
        break;
      case ERROR_SATD :
      default:
        current_intra_sad_2 = distortion_hadamard(pCurImg, curr_mpr_16x16[k]);
        if (img->yuv_format==YUV444 && !IS_INDEPENDENT(input))
        {
          current_intra_sad_2 += distortion_hadamard(imgUV_org[0], img->mpr_16x16[1][k]);
          current_intra_sad_2 += distortion_hadamard(imgUV_org[1], img->mpr_16x16[2][k]);
        }
        break;
      }

      if(current_intra_sad_2 < best_intra_sad2)
      {
        best_intra_sad2 = current_intra_sad_2;
        *intra_mode = k; // update best intra mode
      }
    }
  }

  return best_intra_sad2;
}


/*!
 ************************************************************************
 * \brief
 *    Change color plane for 4:4:4 Independent Mode
 *
 * \par Input:
 *    plane number
 ************************************************************************/
void change_plane_JV( int nplane )
{
  img->colour_plane_id = nplane;
  img->mb_data = img->mb_data_JV[nplane];
  enc_picture = enc_frame_picture_JV[nplane];
  pCurImg = imgY_org_frm_JV[nplane];
  Co_located = Co_located_JV[nplane];
}

/*!
 ************************************************************************
 * \brief
 *    Make one frame picture from 4:4:4 plane
 ************************************************************************/
void make_frame_picture_JV()
{
  int uv, line;
  int nsize;
  int nplane;
  enc_frame_picture[0] = enc_frame_picture_JV[0];

  for( nplane=0; nplane<MAX_PLANE; nplane++ )
  {
    copy_storable_param_JV( nplane, enc_frame_picture[0], enc_frame_picture_JV[nplane] );
  }

  for( uv=0; uv<2; uv++ ){
    for( line=0; line<img->height; line++ )
    {
      nsize = sizeof(imgpel) * img->width;
      memcpy( enc_frame_picture[0]->imgUV[uv][line], enc_frame_picture_JV[uv+1]->imgY[line], nsize );
    }
    free_storable_picture(enc_frame_picture_JV[uv+1]);
  }
}

