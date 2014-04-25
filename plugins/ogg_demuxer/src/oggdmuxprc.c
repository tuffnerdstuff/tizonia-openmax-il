/**
 * Copyright (C) 2011-2014 Aratelia Limited - Juan A. Rubio
 *
 * This file is part of Tizonia
 *
 * Tizonia is free software: you can redistribute it and/or modify it under the
 * terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * Tizonia is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Tizonia.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file   oggdmuxprc.c
 * @author Juan A. Rubio <juan.rubio@aratelia.com>
 *
 * @brief  Tizonia OpenMAX IL - Ogg demuxer processor class implementation
 *
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <assert.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <limits.h>

#include <tizplatform.h>

#include <tizkernel.h>

#include "oggdmux.h"
#include "oggdmuxprc_decls.h"
#include "oggdmuxprc.h"

#ifdef TIZ_LOG_CATEGORY_NAME
#undef TIZ_LOG_CATEGORY_NAME
#define TIZ_LOG_CATEGORY_NAME "tiz.ogg_demuxer.prc"
#endif

#ifdef _DEBUG
static int g_total_read = 0;
static int g_last_read = 0;
static int g_total_released = 0;
#endif

/* Forward declarations */
static OMX_ERRORTYPE oggdmux_prc_deallocate_resources (void *);

static inline bool
is_audio_content (const OggzStreamContent content)
{
  switch (content)
    {
    case OGGZ_CONTENT_VORBIS:
    case OGGZ_CONTENT_SPEEX:
    case OGGZ_CONTENT_PCM:
    case OGGZ_CONTENT_FLAC0:
    case OGGZ_CONTENT_FLAC:
    case OGGZ_CONTENT_CELT:
    case OGGZ_CONTENT_OPUS:
      return true;
    default:
      return false;
    };
}

static inline bool
is_video_content (const OggzStreamContent content)
{
  switch (content)
    {
    case OGGZ_CONTENT_THEORA:
    case OGGZ_CONTENT_VP8:
      return true;
    default:
      return false;
    };
}

static OMX_ERRORTYPE
obtain_uri (oggdmux_prc_t * ap_prc)
{
  OMX_ERRORTYPE rc = OMX_ErrorNone;
  const long pathname_max = PATH_MAX + NAME_MAX;

  assert (NULL != ap_prc);
  assert (NULL == ap_prc->p_uri_param_);

  if (NULL == (ap_prc->p_uri_param_ = tiz_mem_calloc
               (1,
                sizeof (OMX_PARAM_CONTENTURITYPE) + pathname_max + 1)))
    {
      TIZ_ERROR (handleOf (ap_prc),
                 "Error allocating memory for the content uri struct");
      rc = OMX_ErrorInsufficientResources;
    }
  else
    {
      ap_prc->p_uri_param_->nSize = sizeof (OMX_PARAM_CONTENTURITYPE)
        + pathname_max + 1;
      ap_prc->p_uri_param_->nVersion.nVersion = OMX_VERSION;

      if (OMX_ErrorNone != (rc = tiz_api_GetParameter
                            (tiz_get_krn (handleOf (ap_prc)), handleOf (ap_prc),
                             OMX_IndexParamContentURI, ap_prc->p_uri_param_)))
        {
          TIZ_ERROR (handleOf (ap_prc),
                     "[%s] : Error retrieving the URI param from port",
                     tiz_err_to_str (rc));
        }
      else
        {
          TIZ_NOTICE (handleOf (ap_prc), "URI [%s]",
                      ap_prc->p_uri_param_->contentURI);
        }
    }
  return rc;
}

static OMX_ERRORTYPE
alloc_temp_data_stores (oggdmux_prc_t * ap_prc)
{
  OMX_PARAM_PORTDEFINITIONTYPE port_def;

  assert (NULL != ap_prc);

  port_def.nSize = (OMX_U32) sizeof (OMX_PARAM_PORTDEFINITIONTYPE);
  port_def.nVersion.nVersion = OMX_VERSION;
  port_def.nPortIndex = ARATELIA_OGG_DEMUXER_AUDIO_PORT_INDEX;

  tiz_check_omx_err
    (tiz_api_GetParameter (tiz_get_krn (handleOf (ap_prc)), handleOf (ap_prc),
                           OMX_IndexParamPortDefinition, &port_def));
  ap_prc->aud_buf_size_ = port_def.nBufferSize;

  assert (ap_prc->p_aud_store_ == NULL);
  ap_prc->aud_store_size_ = port_def.nBufferSize;
  tiz_check_null_ret_oom ((ap_prc->p_aud_store_ =
                           tiz_mem_alloc (ap_prc->aud_store_size_)));

  port_def.nPortIndex = ARATELIA_OGG_DEMUXER_VIDEO_PORT_INDEX;
  tiz_check_omx_err
    (tiz_api_GetParameter (tiz_get_krn (handleOf (ap_prc)), handleOf (ap_prc),
                           OMX_IndexParamPortDefinition, &port_def));
  ap_prc->vid_buf_size_ = port_def.nBufferSize;

  assert (ap_prc->p_vid_store_ == NULL);
  ap_prc->vid_store_size_ = port_def.nBufferSize;
  tiz_check_null_ret_oom ((ap_prc->p_vid_store_ =
                           tiz_mem_alloc (ap_prc->vid_store_size_)));

  return OMX_ErrorNone;
}

static inline void
close_file ( /*@special@ */ oggdmux_prc_t * ap_prc)
  /*@releases ap_prc->p_file_ @ */
  /*@ensures isnull ap_prc->p_file_@ */
{
  assert (NULL != ap_prc);
  if (NULL != ap_prc->p_file_)
    {
      (void) fclose (ap_prc->p_file_);
      ap_prc->p_file_ = NULL;
    }
}

static inline void
delete_oggz ( /*@special@ */ oggdmux_prc_t * ap_prc)
  /*@releases ap_prc->p_oggz_, ap_prc->p_tracks_ @ */
  /*@ensures isnull ap_prc->p_oggz_, ap_prc->p_tracks_ @ */
{
  assert (NULL != ap_prc);
  oggz_table_delete (ap_prc->p_tracks_);
  ap_prc->p_tracks_ = NULL;
/*   oggz_close (ap_prc->p_oggz_); */
  ap_prc->p_oggz_ = NULL;
}

static inline void
delete_uri ( /*@special@ */ oggdmux_prc_t * ap_prc)
  /*@releases ap_prc->p_uri_param_ @ */
  /*@ensures isnull ap_prc->p_uri_param_ @ */
{
  assert (NULL != ap_prc);
  tiz_mem_free (ap_prc->p_uri_param_);
  ap_prc->p_uri_param_ = NULL;
}

static inline void
dealloc_temp_data_stores ( /*@special@ */ oggdmux_prc_t * ap_prc)
  /*@releases ap_prc->p_aud_store_, ap_prc->p_vid_store_ @ */
  /*@ensures isnull ap_prc->p_aud_store_, ap_prc->p_vid_store_ @ */
{
  assert (NULL != ap_prc);
  tiz_mem_free (ap_prc->p_aud_store_);
  tiz_mem_free (ap_prc->p_vid_store_);
  ap_prc->p_aud_store_ = NULL;
  ap_prc->p_vid_store_ = NULL;
  ap_prc->aud_store_size_ = 0;
  ap_prc->vid_store_size_ = 0;
  ap_prc->aud_store_offset_ = 0;
  ap_prc->vid_store_offset_ = 0;
}

static inline OMX_U8 **
get_store_ptr (oggdmux_prc_t * ap_prc, const OMX_U32 a_pid)
{
  OMX_U8 **pp_store = NULL;
  assert (NULL != ap_prc);
  assert (a_pid <= ARATELIA_OGG_DEMUXER_VIDEO_PORT_INDEX);
  pp_store = a_pid == ARATELIA_OGG_DEMUXER_AUDIO_PORT_INDEX
    ? &(ap_prc->p_aud_store_) : &(ap_prc->p_vid_store_);
  return pp_store;
}

static inline OMX_U32 *
get_store_size_ptr (oggdmux_prc_t * ap_prc, const OMX_U32 a_pid)
{
  OMX_U32 *p_size = NULL;
  assert (NULL != ap_prc);
  assert (a_pid <= ARATELIA_OGG_DEMUXER_VIDEO_PORT_INDEX);
  p_size = a_pid == ARATELIA_OGG_DEMUXER_AUDIO_PORT_INDEX
    ? &(ap_prc->aud_store_size_) : &(ap_prc->vid_store_size_);
  return p_size;
}

static inline OMX_U32 *
get_store_offset_ptr (oggdmux_prc_t * ap_prc, const OMX_U32 a_pid)
{
  OMX_U32 *p_offset = NULL;
  assert (NULL != ap_prc);
  assert (a_pid <= ARATELIA_OGG_DEMUXER_VIDEO_PORT_INDEX);
  p_offset = a_pid == ARATELIA_OGG_DEMUXER_AUDIO_PORT_INDEX
    ? &(ap_prc->aud_store_offset_) : &(ap_prc->vid_store_offset_);
  assert (NULL != p_offset);
  return p_offset;
}

static inline bool *
get_port_disabled_ptr (oggdmux_prc_t * ap_prc, const OMX_U32 a_pid)
{
  bool *p_port_disabled = NULL;
  assert (NULL != ap_prc);
  assert (a_pid <= ARATELIA_OGG_DEMUXER_VIDEO_PORT_INDEX);
  p_port_disabled = (a_pid == ARATELIA_OGG_DEMUXER_AUDIO_PORT_INDEX
                     ? &(ap_prc->aud_port_disabled_)
                     : &(ap_prc->vid_port_disabled_));
  assert (NULL != p_port_disabled);
  return p_port_disabled;
}

static inline OMX_BUFFERHEADERTYPE **
get_header_ptr (oggdmux_prc_t * ap_prc, const OMX_U32 a_pid)
{
  OMX_BUFFERHEADERTYPE **pp_hdr = NULL;
  assert (NULL != ap_prc);
  assert (a_pid <= ARATELIA_OGG_DEMUXER_VIDEO_PORT_INDEX);
  pp_hdr = (a_pid == ARATELIA_OGG_DEMUXER_AUDIO_PORT_INDEX
            ? &(ap_prc->p_aud_hdr_) : &(ap_prc->p_vid_hdr_));
  assert (NULL != pp_hdr);
  return pp_hdr;
}

static inline bool *
get_eos_ptr (oggdmux_prc_t * ap_prc, const OMX_U32 a_pid)
{
  bool *p_eos = NULL;
  assert (NULL != ap_prc);
  assert (a_pid <= ARATELIA_OGG_DEMUXER_VIDEO_PORT_INDEX);
  p_eos = (a_pid == ARATELIA_OGG_DEMUXER_AUDIO_PORT_INDEX
           ? &(ap_prc->aud_eos_) : &(ap_prc->vid_eos_));
  assert (NULL != p_eos);
  return p_eos;
}

static int
store_data (oggdmux_prc_t * ap_prc, const OMX_U32 a_pid,
            const OMX_U8 * ap_data, OMX_U32 a_nbytes)
{
  OMX_U8 **pp_store = NULL;
  OMX_U32 *p_offset = NULL;
  OMX_U32 *p_size = NULL;
  OMX_U32 nbytes_to_copy = 0;
  OMX_U32 nbytes_avail = 0;

  assert (NULL != ap_prc);
  assert (a_pid <= ARATELIA_OGG_DEMUXER_VIDEO_PORT_INDEX);
  assert (NULL != ap_data);

  pp_store = get_store_ptr (ap_prc, a_pid);
  p_size = get_store_size_ptr (ap_prc, a_pid);
  p_offset = get_store_offset_ptr (ap_prc, a_pid);

  assert (NULL != pp_store && NULL != *pp_store);
  assert (NULL != p_size);
  assert (NULL != p_offset);

  nbytes_avail = *p_size - *p_offset;

  if (a_nbytes > nbytes_avail)
    {
      /* need to re-alloc */
      OMX_U8 *p_new_store = NULL;
      p_new_store = tiz_mem_realloc (*pp_store, *p_offset + a_nbytes);
      if (NULL != p_new_store)
        {
          *pp_store = p_new_store;
          *p_size = *p_offset + a_nbytes;
          nbytes_avail = *p_size - *p_offset;
          TIZ_TRACE (handleOf (ap_prc), "pid [%d] : Realloc'd data store "
                     "to new size [%d]", a_pid, *p_size);
        }
    }
  nbytes_to_copy = MIN (nbytes_avail, a_nbytes);
  memcpy (*pp_store + *p_offset, ap_data, nbytes_to_copy);
  *p_offset += nbytes_to_copy;

  TIZ_TRACE (handleOf (ap_prc),
             "pid [%d]: bytes currently stored [%d]", a_pid, *p_offset);

  return a_nbytes - nbytes_to_copy;
}

static int
dump_temp_store (oggdmux_prc_t * ap_prc,
                 const OMX_U32 a_pid, OMX_BUFFERHEADERTYPE * ap_hdr)
{
  OMX_U8 *p_store = NULL;
  OMX_U32 *p_offset = NULL;
  OMX_U32 nbytes_to_copy = 0;
  OMX_U32 nbytes_avail = 0;

  assert (NULL != ap_prc);
  assert (a_pid <= ARATELIA_OGG_DEMUXER_VIDEO_PORT_INDEX);
  assert (NULL != ap_hdr);

  p_store = *(get_store_ptr (ap_prc, a_pid));
  p_offset = get_store_offset_ptr (ap_prc, a_pid);

  assert (NULL != p_store);
  assert (NULL != p_offset);
  assert (ap_hdr->nAllocLen >= ap_hdr->nFilledLen);

  nbytes_avail = ap_hdr->nAllocLen - ap_hdr->nFilledLen;
  nbytes_to_copy = MIN (*p_offset, nbytes_avail);

  if (nbytes_to_copy > 0)
    {
      memcpy (ap_hdr->pBuffer + ap_hdr->nFilledLen, p_store, nbytes_to_copy);
      ap_hdr->nFilledLen += nbytes_to_copy;
      *p_offset -= nbytes_to_copy;
      if (*p_offset > 0)
        {
          memmove (p_store, p_store + nbytes_to_copy, *p_offset);
        }
      TIZ_TRACE (handleOf (ap_prc), "HEADER [%p] pid [%d] nFilledLen [%d] "
                 "offset [%d]", ap_hdr, a_pid, ap_hdr->nFilledLen, *p_offset);
    }

  return *p_offset;
}

static OMX_U32
dump_ogg_data (oggdmux_prc_t * ap_prc,
               const OMX_U32 a_pid,
               const OMX_U8 * ap_ogg_data,
               const OMX_U32 a_nbytes, OMX_BUFFERHEADERTYPE * ap_hdr)
{
  OMX_U32 nbytes_copied = 0;
  OMX_U32 nbytes_avail = 0;

  assert (NULL != ap_prc);
  assert (a_pid <= ARATELIA_OGG_DEMUXER_VIDEO_PORT_INDEX);
  assert (NULL != ap_ogg_data);
  assert (NULL != ap_hdr);

  assert (ap_hdr->nAllocLen >= ap_hdr->nFilledLen);

  nbytes_avail = ap_hdr->nAllocLen - ap_hdr->nFilledLen;
  nbytes_copied = MIN (a_nbytes, nbytes_avail);

  if (nbytes_copied > 0)
    {
      memcpy (ap_hdr->pBuffer + ap_hdr->nFilledLen,
              ap_ogg_data, nbytes_copied);
      ap_hdr->nFilledLen += nbytes_copied;
    }

  TIZ_TRACE (handleOf (ap_prc), "HEADER [%p] pid [%d] nbytes [%d] "
             "nbytes_copied [%d] nFilledLen [%d]",
             ap_hdr, a_pid, a_nbytes, nbytes_copied, ap_hdr->nFilledLen);

  return nbytes_copied;
}

static OMX_BUFFERHEADERTYPE *
get_buffer (oggdmux_prc_t * ap_prc, const OMX_U32 a_pid)
{
  OMX_BUFFERHEADERTYPE **pp_hdr = get_header_ptr (ap_prc, a_pid);
  bool *p_port_disabled = get_port_disabled_ptr (ap_prc, a_pid);
  assert (NULL != ap_prc);

  if (false == *p_port_disabled)
    {
      if (NULL != *pp_hdr)
        {
          TIZ_TRACE (handleOf (ap_prc), "HEADER [%p] pid [%d] "
                     "nFilledLen [%d] ", *pp_hdr, a_pid,
                     (*pp_hdr)->nFilledLen);
          return *pp_hdr;
        }
      else
        {
          if (OMX_ErrorNone == tiz_krn_claim_buffer
              (tiz_get_krn (handleOf (ap_prc)), a_pid, 0, pp_hdr))
            {
              if (NULL != *pp_hdr)
                {
                  TIZ_TRACE (handleOf (ap_prc), "Claimed HEADER [%p] "
                             "pid [%d] nFilledLen [%d]",
                             *pp_hdr, a_pid, (*pp_hdr)->nFilledLen);
                  return *pp_hdr;
                }
            }
        }
      ap_prc->awaiting_buffers_ = true;
    }

  return NULL;
}

/* TODO: Change void to a int for OOM errors */
static void
release_buffer (oggdmux_prc_t * ap_prc, const OMX_U32 a_pid)
{
  OMX_BUFFERHEADERTYPE **pp_hdr = get_header_ptr (ap_prc, a_pid);
  OMX_BUFFERHEADERTYPE *p_hdr = NULL;

  assert (NULL != ap_prc);

  p_hdr = *pp_hdr;
  assert (NULL != p_hdr);

  p_hdr->nOffset = 0;

  TIZ_TRACE (handleOf (ap_prc), "Releasing HEADER [%p] pid [%d] "
             "nFilledLen [%d] nFlags [%d]", p_hdr, a_pid, p_hdr->nFilledLen,
             p_hdr->nFlags);

  /* TODO: Check for OOM error and issue Error Event */
  (void) tiz_krn_release_buffer
    (tiz_get_krn (handleOf (ap_prc)), a_pid, p_hdr);
  *pp_hdr = NULL;

  ap_prc->awaiting_buffers_ = true;
}

static bool
release_buffer_with_eos (oggdmux_prc_t * ap_prc, const OMX_U32 a_pid)
{
  bool eos_released = false;
  OMX_BUFFERHEADERTYPE *p_hdr = get_buffer (ap_prc, a_pid);

  if (NULL != p_hdr)
    {
      TIZ_TRACE (handleOf (ap_prc), "Adding EOS flag - PID [%d]", a_pid);
      p_hdr->nFlags |= OMX_BUFFERFLAG_EOS;
      release_buffer (ap_prc, a_pid);
      eos_released = true;
    }
  return eos_released;
}

static int
flush_temp_store (oggdmux_prc_t * ap_prc, const OMX_U32 a_pid)
{
  OMX_BUFFERHEADERTYPE *p_hdr = NULL;
  OMX_U32 *p_offset = get_store_offset_ptr (ap_prc, a_pid);
  OMX_U32 ds_offset = *p_offset;

  assert (NULL != p_offset);

  if (0 == *p_offset)
    {
      /* The temp store is empty */
      return 0;
    }

  while (NULL != (p_hdr = get_buffer (ap_prc, a_pid)))
    {
      ds_offset = dump_temp_store (ap_prc, a_pid, p_hdr);
#ifdef _DEBUG
      if (a_pid == ARATELIA_OGG_DEMUXER_AUDIO_PORT_INDEX)
        {
          g_total_released += p_hdr->nFilledLen;
          OMX_U32 *p_offset = get_store_offset_ptr (ap_prc, a_pid);
          TIZ_TRACE (handleOf (ap_prc), "total released [%d] "
                     "total read [%d] store [%d] last read [%d] diff [%d]",
                     g_total_released, g_total_read, *p_offset, g_last_read,
                     g_total_read - (g_total_released + *p_offset));
        }
#endif
      if (ap_prc->file_eos_ && 0 == ds_offset)
        {
          bool *p_eos = get_eos_ptr (ap_prc, a_pid);
          if (!p_eos)
            {
              TIZ_DEBUG (handleOf (ap_prc), "Adding EOS flag - PID [%d]",
                         a_pid);
              p_hdr->nFlags |= OMX_BUFFERFLAG_EOS;
              *p_eos = true;
            }
        }
      release_buffer (ap_prc, a_pid);
      p_hdr = 0;
      if (0 == ds_offset)
        {
          break;
        }
    }
  return ds_offset;
}

static int
flush_ogg_packet (oggdmux_prc_t * ap_prc, const OMX_U32 a_pid,
                  const OMX_U8 * ap_ogg_data, const OMX_U32 nbytes)
{
  OMX_BUFFERHEADERTYPE *p_hdr = NULL;
  OMX_U32 nbytes_remaining = nbytes;
  OMX_U32 nbytes_copied = 0;
  OMX_U32 op_offset = 0;

  while (NULL != (p_hdr = get_buffer (ap_prc, a_pid)))
    {
      nbytes_copied
        = dump_ogg_data (ap_prc, a_pid, ap_ogg_data + op_offset,
                         nbytes_remaining, p_hdr);
      nbytes_remaining -= nbytes_copied;
      op_offset += nbytes_copied;
#ifdef _DEBUG
      if (a_pid == ARATELIA_OGG_DEMUXER_AUDIO_PORT_INDEX)
        {
          g_total_released += p_hdr->nFilledLen;
          OMX_U32 *p_offset = get_store_offset_ptr (ap_prc, a_pid);
          TIZ_TRACE (handleOf (ap_prc), "total released [%d] "
                     "total read [%d] store [%d] last read [%d] "
                     "remaining [%d] diff [%d]",
                     g_total_released, g_total_read, *p_offset,
                     g_last_read, nbytes_remaining,
                     g_total_read - (g_total_released + nbytes_remaining));
        }
#endif
      release_buffer (ap_prc, a_pid);
      p_hdr = 0;
      if (0 == nbytes_remaining)
        {
          break;
        }
    }

  if (nbytes_remaining > 0)
    {
      /* Need_more_buffers. Temporarily store the data until an omx buffer is
       * available */
      TIZ_TRACE (handleOf (ap_prc), "Need to store [%d] bytes - pid [%d]",
                 nbytes_remaining, a_pid);
      nbytes_remaining = store_data (ap_prc, a_pid, ap_ogg_data + op_offset,
                                     nbytes_remaining);
    }

  return nbytes_remaining;
}

static inline int
flush_stores (oggdmux_prc_t * ap_prc)
{
  int remaining = 0;
  remaining = flush_temp_store (ap_prc, ARATELIA_OGG_DEMUXER_AUDIO_PORT_INDEX);
  remaining +=
    flush_temp_store (ap_prc, ARATELIA_OGG_DEMUXER_VIDEO_PORT_INDEX);
  return remaining;
}

static int
read_packet (OGGZ * ap_oggz, oggz_packet * ap_zp, long serialno,
             void *ap_user_data, const OMX_U32 a_pid)
{
  oggdmux_prc_t *p_prc = ap_user_data;
  OMX_U32 op_offset = 0;
  bool *p_eos = NULL;
  ogg_packet *p_op = NULL;
  int rc = OGGZ_CONTINUE;

  assert (NULL != ap_oggz);
  assert (NULL != ap_zp);
  assert (NULL != p_prc);
  assert (a_pid <= ARATELIA_OGG_DEMUXER_VIDEO_PORT_INDEX);

  p_op = &(ap_zp->op);
  p_eos = get_eos_ptr (p_prc, a_pid);

  TIZ_TRACE (handleOf (p_prc), "%010lu: pid [%d] reading bytes [%d]",
             serialno, a_pid, p_op->bytes);

#ifdef _DEBUG
  if (a_pid == ARATELIA_OGG_DEMUXER_AUDIO_PORT_INDEX)
    {
      g_total_read += p_op->bytes;
      g_last_read = p_op->bytes;
    }
#endif

  if (oggz_get_eos (ap_oggz, serialno) == 1)
    {
      TIZ_TRACE (handleOf (p_prc), "%010lu: This is EOS\n", serialno);
      *p_eos = true;
    }

  /* Try to empty the ogg packet out to an omx buffer */
  op_offset = flush_ogg_packet (p_prc, a_pid, p_op->packet, p_op->bytes);

  if (0 == op_offset)
    {
      if (*p_eos || !get_buffer (p_prc, a_pid) ||
          (*get_store_offset_ptr (p_prc, a_pid)) > 0)
        {
          rc = OGGZ_STOP_OK;
        }
      else
        {
          rc = OGGZ_CONTINUE;
        }
    }
  else                          /* (op_offset != 0) */
    {
      rc = OGGZ_STOP_ERR;
    }

  TIZ_TRACE (handleOf (p_prc), "%010lu: rc [%d] op_offset [%d]",
             serialno, rc, op_offset);

  return rc;
}

static int
read_audio_packet (OGGZ * ap_oggz, oggz_packet * ap_zp, long serialno,
                   void *ap_user_data)
{
  oggdmux_prc_t *p_prc = ap_user_data;
  int rc = OGGZ_CONTINUE;
  assert (NULL != ap_user_data);

  if (!p_prc->aud_port_disabled_
      && is_audio_content (oggz_stream_get_content (p_prc->p_oggz_, serialno)))
    {
      rc = read_packet (ap_oggz, ap_zp, serialno, ap_user_data,
                        ARATELIA_OGG_DEMUXER_AUDIO_PORT_INDEX);
    }
  TIZ_TRACE (handleOf (p_prc), "%010lu: rc [%d]", serialno, rc);
  return rc;
}

static int
read_video_packet (OGGZ * ap_oggz, oggz_packet * ap_zp, long serialno,
                   void *ap_user_data)
{
  oggdmux_prc_t *p_prc = ap_user_data;
  int rc = OGGZ_CONTINUE;
  assert (NULL != ap_user_data);

  if (!p_prc->vid_port_disabled_
      && is_video_content (oggz_stream_get_content (p_prc->p_oggz_, serialno)))
    {
      TIZ_TRACE (handleOf (p_prc), "Called read_video_packet callback");
      rc = read_packet (ap_oggz, ap_zp, serialno, ap_user_data,
                        ARATELIA_OGG_DEMUXER_VIDEO_PORT_INDEX);
    }
  TIZ_TRACE (handleOf (p_prc), "%010lu: rc [%d]", serialno, rc);
  return rc;
}

static OMX_ERRORTYPE
release_all_buffers (oggdmux_prc_t * ap_prc, const OMX_U32 a_pid)
{
  assert (NULL != ap_prc);

  if ((a_pid == ARATELIA_OGG_DEMUXER_AUDIO_PORT_INDEX
       || a_pid == OMX_ALL) && (NULL != ap_prc->p_aud_hdr_))
    {
      void *p_krn = tiz_get_krn (handleOf (ap_prc));
      tiz_check_omx_err
        (tiz_krn_release_buffer (p_krn,
                                 ARATELIA_OGG_DEMUXER_AUDIO_PORT_INDEX,
                                 ap_prc->p_aud_hdr_));
      ap_prc->p_aud_hdr_ = NULL;
    }

  if ((a_pid == ARATELIA_OGG_DEMUXER_VIDEO_PORT_INDEX
       || a_pid == OMX_ALL) && (NULL != ap_prc->p_vid_hdr_))
    {
      void *p_krn = tiz_get_krn (handleOf (ap_prc));
      tiz_check_omx_err
        (tiz_krn_release_buffer (p_krn,
                                 ARATELIA_OGG_DEMUXER_VIDEO_PORT_INDEX,
                                 ap_prc->p_vid_hdr_));
      ap_prc->p_vid_hdr_ = NULL;
    }

  ap_prc->awaiting_buffers_ = true;

  return OMX_ErrorNone;
}

static inline OMX_ERRORTYPE
do_flush (oggdmux_prc_t * ap_prc)
{
  assert (NULL != ap_prc);
  TIZ_TRACE (handleOf (ap_prc), "do_flush");
  (void) oggz_purge (ap_prc->p_oggz_);
  ap_prc->aud_store_offset_ = 0;
  ap_prc->vid_store_offset_ = 0;
  /* Release any buffers held  */
  return release_all_buffers (ap_prc, OMX_ALL);
}

static size_t
io_read (void *ap_user_handle, void *ap_buf, size_t n)
{
  oggdmux_prc_t *p_prc = ap_user_handle;
  FILE *f = NULL;
  ssize_t bytes_read = 0;

  assert (NULL != p_prc);
  f = p_prc->p_file_;

  bytes_read = read (fileno (f), ap_buf, n);
  if (0 == bytes_read)
    {
      TIZ_TRACE (handleOf (p_prc), "Zero bytes_read buf [%p] n [%d]",
                 ap_buf, n);
    }
  return bytes_read;
}

static int
io_seek (void *ap_user_handle, long offset, int whence)
{
  oggdmux_prc_t *p_prc = ap_user_handle;
  FILE *f = NULL;
  assert (NULL != p_prc);
  f = p_prc->p_file_;
  return (fseek (f, offset, whence));
}

static long
io_tell (void *ap_user_handle)
{
  oggdmux_prc_t *p_prc = ap_user_handle;
  FILE *f = NULL;
  assert (NULL != p_prc);
  f = p_prc->p_file_;
  return ftell (f);
}

static int
read_page_normal (OGGZ * ap_oggz, const ogg_page * ap_og,
                  long a_serialno, void *ap_user_data)
{
  oggdmux_prc_t *p_prc = ap_user_data;
  TIZ_DEBUG (handleOf (p_prc), "serialno = [%d] : granule pos [%lld]",
             a_serialno, oggz_tell_granulepos (ap_oggz));
  return OGGZ_CONTINUE;
}

static int
read_page_first_pass (OGGZ * ap_oggz, const ogg_page * ap_og,
                      long a_serialno, void *ap_user_data)
{
  int rc = OGGZ_CONTINUE;
  oggdmux_prc_t *p_prc = ap_user_data;
  assert (NULL != p_prc);
  assert (NULL != ap_oggz);

  if (oggz_get_bos (ap_oggz, a_serialno) > 0)
    {
      TIZ_TRACE (handleOf (p_prc), "serialno = [%d]", a_serialno);
      if (NULL == oggz_table_insert (p_prc->p_tracks_, a_serialno, &read_page_first_pass))      /* NULL makes it barf, needs
                                                                                                 * something */
        {
          TIZ_ERROR (handleOf (p_prc), "serialno = [%d] - "
                     "Could not insert serialno in oggz table", a_serialno);
          rc = OGGZ_STOP_ERR;
        }
    }

  if (oggz_get_bos (ap_oggz, ALL_OGG_STREAMS) == 0)
    {
      TIZ_TRACE (handleOf (p_prc),
                 "Number of tracks [%d]", oggz_get_numtracks (ap_oggz));
      return OGGZ_STOP_OK;
    }

  return rc;
}

static inline OMX_ERRORTYPE
seek_to_byte_offset (oggdmux_prc_t * ap_prc, const oggz_off_t a_offset)
{
  assert (NULL != ap_prc);
  /* Reset the internal EOS flags */
  ap_prc->aud_eos_ = false;
  ap_prc->vid_eos_ = false;
  if (oggz_seek (ap_prc->p_oggz_, a_offset, SEEK_SET) == -1)
    {
      TIZ_ERROR (handleOf (ap_prc), "[OMX_ErrorInsufficientResources] : "
                 "Could not seek to [%d] offset", a_offset);
      return OMX_ErrorInsufficientResources;
    }
  return OMX_ErrorNone;
}

static inline OMX_ERRORTYPE
set_read_page_callback (oggdmux_prc_t * ap_prc, OggzReadPage ap_read_cback)
{
  assert (NULL != ap_prc);
  assert (NULL != ap_read_cback);
  if (oggz_set_read_page (ap_prc->p_oggz_, ALL_OGG_STREAMS,
                          ap_read_cback, ap_prc) < 0)
    {
      TIZ_ERROR (handleOf (ap_prc), "[OMX_ErrorInsufficientResources] : "
                 "Could not set read page callback.");
      return OMX_ErrorInsufficientResources;
    }
  return OMX_ErrorNone;
}

static inline OMX_ERRORTYPE
set_read_packet_callback (oggdmux_prc_t * ap_prc, long serialno,
                          OggzReadPacket ap_read_cback)
{
  assert (NULL != ap_prc);
  assert (NULL != ap_read_cback);
  if (oggz_set_read_callback (ap_prc->p_oggz_, serialno,
                              ap_read_cback, ap_prc) < 0)
    {
      TIZ_ERROR (handleOf (ap_prc), "[OMX_ErrorInsufficientResources] : "
                 "Could not set read packet callback.");
      return OMX_ErrorInsufficientResources;
    }
  return OMX_ErrorNone;
}

static OMX_ERRORTYPE
obtain_tracks (oggdmux_prc_t * ap_prc)
{
  oggdmux_prc_t *p_prc = ap_prc;
  long n = 0;
  assert (NULL != p_prc);

  /* Seek to beginning of file and set the first pass callback that will help
   * with the discovery of the codecs */
  tiz_check_omx_err (seek_to_byte_offset (p_prc, 0));
  tiz_check_omx_err (set_read_page_callback (p_prc, read_page_first_pass));

  while ((n = oggz_read (p_prc->p_oggz_,
                         TIZ_OGG_DEMUXER_INITIAL_READ_BLOCKSIZE)) > 0);

  /* Seek to beginning of file and set the normal callback (no-op function) */
  tiz_check_omx_err (seek_to_byte_offset (p_prc, 0));
  tiz_check_omx_err (set_read_page_callback (p_prc, read_page_normal));

  return OMX_ErrorNone;
}

static void
print_codec_name (oggdmux_prc_t * ap_prc, long serialno)
{
  if (NULL != ap_prc)
    {
      OggzStreamContent content = oggz_stream_get_content (ap_prc->p_oggz_,
                                                           serialno);
      const char *p_codec_name = oggz_content_type (content);
      TIZ_TRACE (handleOf (ap_prc), "%010lu: codec [%s]", serialno,
                 p_codec_name != NULL ? p_codec_name : "Unknown");
    }
}

static OMX_ERRORTYPE
set_read_packet_callbacks (oggdmux_prc_t * ap_prc)
{
  OMX_ERRORTYPE rc = OMX_ErrorNone;
  long serialno = 0;
  int n = 0;
  int i = 0;
  void *p_nth_data = NULL;

  assert (NULL != ap_prc);

  n = oggz_table_size (ap_prc->p_tracks_);
  TIZ_TRACE (handleOf (ap_prc), "oggz table size [%d]", n);
  for (i = 0; i < n; i++)
    {
      OggzStreamContent content = OGGZ_CONTENT_UNKNOWN;
      p_nth_data = oggz_table_nth (ap_prc->p_tracks_, i, &serialno);
      assert (NULL != p_nth_data);
      print_codec_name (ap_prc, serialno);
      content = oggz_stream_get_content (ap_prc->p_oggz_, serialno);
      if (is_audio_content (content))
        {
          TIZ_TRACE (handleOf (ap_prc), "Set read_audio_packet callback");
          tiz_check_omx_err
            (set_read_packet_callback (ap_prc, serialno, read_audio_packet));
        }

      if (is_video_content (content))
        {
          TIZ_TRACE (handleOf (ap_prc), "Set read_video_packet callback");
          tiz_check_omx_err
            (set_read_packet_callback (ap_prc, serialno, read_video_packet));
        }
    }

  if (0 == n)
    {
      rc = OMX_ErrorFormatNotDetected;
    }

  return rc;
}

static inline bool
buffers_available (oggdmux_prc_t * ap_prc)
{
  bool rc = false;
  bool aud_port_enabled =
    !(*get_port_disabled_ptr (ap_prc, ARATELIA_OGG_DEMUXER_AUDIO_PORT_INDEX));
  bool vid_port_enabled =
    !(*get_port_disabled_ptr (ap_prc, ARATELIA_OGG_DEMUXER_VIDEO_PORT_INDEX));
  if (aud_port_enabled)
    {
      rc |=
        (NULL != get_buffer (ap_prc, ARATELIA_OGG_DEMUXER_AUDIO_PORT_INDEX));
    }
  if (vid_port_enabled)
    {
      rc |=
        (NULL != get_buffer (ap_prc, ARATELIA_OGG_DEMUXER_VIDEO_PORT_INDEX));
    }
  return rc;
}

static OMX_ERRORTYPE
demux_file (oggdmux_prc_t * ap_prc)
{
  int run_status = 0;
  assert (NULL != ap_prc);

  do
    {
      run_status =
        oggz_read (ap_prc->p_oggz_, TIZ_OGG_DEMUXER_DEFAULT_READ_BLOCKSIZE);
      TIZ_TRACE (handleOf (ap_prc), "run_status [%d]", run_status);
    }
  while (buffers_available (ap_prc) && run_status > 0);

  if (0 == run_status)          /* This indicates end of file */
    {
      int                   remaining = 0;
      ap_prc->file_eos_               = true;
      /* Try to empty the temp stores out to an omx buffer */
      remaining                       = flush_stores (ap_prc);
      TIZ_TRACE (handleOf (ap_prc),
                 "aud_store_offset [%d] vid_store_offset [%d] - total [%d]",
                 ap_prc->aud_store_offset_, ap_prc->vid_store_offset_,
                 remaining);

      if (!ap_prc->aud_eos_)
        {
          ap_prc->aud_eos_ = release_buffer_with_eos
            (ap_prc, ARATELIA_OGG_DEMUXER_AUDIO_PORT_INDEX);
        }
      if (!ap_prc->vid_eos_)
        {
          ap_prc->vid_eos_ = release_buffer_with_eos
            (ap_prc, ARATELIA_OGG_DEMUXER_VIDEO_PORT_INDEX);
        }
    }

  return OMX_ErrorNone;
}

/*
 * oggdmuxprc
 */

static void *
oggdmux_prc_ctor (void *ap_obj, va_list * app)
{
  oggdmux_prc_t *p_prc =
    super_ctor (typeOf (ap_obj, "oggdmuxprc"), ap_obj, app);
  assert (NULL != p_prc);
  p_prc->p_file_ = NULL;
  p_prc->p_uri_param_ = NULL;
  p_prc->p_oggz_ = NULL;
  p_prc->p_tracks_ = NULL;
  p_prc->p_aud_hdr_ = NULL;
  p_prc->p_vid_hdr_ = NULL;
  p_prc->aud_buf_size_ = 0;
  p_prc->vid_buf_size_ = 0;
  p_prc->awaiting_buffers_ = true;
  p_prc->p_aud_store_ = NULL;
  p_prc->p_vid_store_ = NULL;
  p_prc->aud_store_size_ = 0;
  p_prc->vid_store_size_ = 0;
  p_prc->aud_store_offset_ = 0;
  p_prc->vid_store_offset_ = 0;
  p_prc->file_eos_ = false;
  p_prc->aud_eos_ = false;
  p_prc->vid_eos_ = false;
  p_prc->aud_port_disabled_ = false;
  p_prc->vid_port_disabled_ = false;

  return p_prc;
}

static void *
oggdmux_prc_dtor (void *ap_obj)
{
  (void) oggdmux_prc_deallocate_resources (ap_obj);
  return super_dtor (typeOf (ap_obj, "oggdmuxprc"), ap_obj);
}

/*
 * from tizsrv class
 */
static OMX_ERRORTYPE
oggdmux_prc_allocate_resources (void *ap_obj, OMX_U32 a_pid)
{
  oggdmux_prc_t *p_prc = ap_obj;
  assert (NULL != p_prc);
  assert (NULL == p_prc->p_oggz_);
  assert (NULL == p_prc->p_uri_param_);

  tiz_check_omx_err (obtain_uri (p_prc));

  TIZ_TRACE (handleOf (p_prc), "Allocating resources");

  tiz_check_omx_err (alloc_temp_data_stores (p_prc));

  if ((p_prc->p_file_
       = fopen ((const char *) p_prc->p_uri_param_->contentURI, "r")) == 0)
    {
      TIZ_ERROR (handleOf (p_prc),
                 "Error opening file from URI (%s)", strerror (errno));
      return OMX_ErrorInsufficientResources;
    }

  if (NULL == (p_prc->p_oggz_ = oggz_new (OGGZ_READ)))
    {
      TIZ_ERROR (handleOf (p_prc),
                 "Cannot create a new oggz object (%s)", strerror (errno));
      return OMX_ErrorInsufficientResources;
    }

  if (NULL == (p_prc->p_tracks_ = oggz_table_new ()))
    {
      TIZ_ERROR (handleOf (p_prc), "Cannot create a new oggz object");
      return OMX_ErrorInsufficientResources;
    }

  if (oggz_io_set_read (p_prc->p_oggz_, io_read, p_prc) != 0)
    {
      TIZ_ERROR (handleOf (p_prc), "[OMX_ErrorInsufficientResources] : "
                 "Cannot set the oggz io read callback");
      return OMX_ErrorInsufficientResources;
    }

  if (oggz_io_set_seek (p_prc->p_oggz_, io_seek, p_prc) != 0)
    {
      TIZ_ERROR (handleOf (p_prc), "[OMX_ErrorInsufficientResources] : "
                 "Cannot set the oggz io seek callback");
      return OMX_ErrorInsufficientResources;
    }

  if (oggz_io_set_tell (p_prc->p_oggz_, io_tell, p_prc) != 0)
    {
      TIZ_ERROR (handleOf (p_prc), "[OMX_ErrorInsufficientResources] : "
                 "Cannot set the oggz io tell callback");
      return OMX_ErrorInsufficientResources;
    }

  return OMX_ErrorNone;
}

static OMX_ERRORTYPE
oggdmux_prc_deallocate_resources (void *ap_obj)
{
  oggdmux_prc_t *p_prc = ap_obj;
  assert (NULL != p_prc);
  TIZ_TRACE (handleOf (p_prc), "Deallocating resources");
  close_file (p_prc);
  delete_oggz (p_prc);
  delete_uri (p_prc);
  dealloc_temp_data_stores (p_prc);
  return OMX_ErrorNone;
}

static OMX_ERRORTYPE
oggdmux_prc_prepare_to_transfer (void *ap_obj, OMX_U32 a_pid)
{
  oggdmux_prc_t *p_prc = ap_obj;
  assert (NULL != p_prc);
  tiz_check_omx_err (obtain_tracks (p_prc));
  tiz_check_omx_err (set_read_packet_callbacks (p_prc));
  return OMX_ErrorNone;
}

static OMX_ERRORTYPE
oggdmux_prc_transfer_and_process (void *ap_obj, OMX_U32 a_pid)
{
  return OMX_ErrorNone;
}

static OMX_ERRORTYPE
oggdmux_prc_stop_and_return (void *ap_obj)
{
  oggdmux_prc_t *p_prc = ap_obj;
  assert (NULL != p_prc);
  /* Reset the internal EOS flags */
  p_prc->aud_eos_ = false;
  p_prc->vid_eos_ = false;
  p_prc->file_eos_ = false;
  TIZ_TRACE (handleOf (p_prc), "stop_and_return");
  return do_flush (p_prc);
}

/*
 * from tizprc class
 */

static OMX_ERRORTYPE
oggdmux_prc_buffers_ready (const void *ap_obj)
{
  oggdmux_prc_t *p_prc = (oggdmux_prc_t *) ap_obj;
  OMX_ERRORTYPE rc = OMX_ErrorNone;
  assert (NULL != p_prc);
  TIZ_TRACE (handleOf (p_prc), "awaiting_buffers [%s] aud eos [%s] "
             "vid eos [%s]",
             p_prc->awaiting_buffers_ ? "YES" : "NO",
             p_prc->aud_eos_ ? "YES" : "NO", p_prc->vid_eos_ ? "YES" : "NO");

  if (p_prc->awaiting_buffers_ && (!p_prc->aud_eos_ || !p_prc->vid_eos_))
    {
      /* Make sure we have flushed the temp buffers before reading any more
       * data from file */
      if (flush_stores (p_prc) == 0)
        {
          p_prc->awaiting_buffers_ = true;
          rc = demux_file (p_prc);
        }
    }

  return rc;
}

static OMX_ERRORTYPE
oggdmux_prc_port_flush (const void *ap_obj, OMX_U32 TIZ_UNUSED (a_pid))
{
  oggdmux_prc_t *p_prc = (oggdmux_prc_t *) ap_obj;
  assert (NULL != p_prc);
  return do_flush (p_prc);
}

static OMX_ERRORTYPE
oggdmux_prc_port_disable (const void *ap_obj, OMX_U32 a_pid)
{
  oggdmux_prc_t *p_prc = (oggdmux_prc_t *) ap_obj;
  bool *p_port_disabled = NULL;
  assert (NULL != p_prc);
  assert (a_pid <= ARATELIA_OGG_DEMUXER_VIDEO_PORT_INDEX);

  if (OMX_ALL == a_pid)
    {
      p_port_disabled = get_port_disabled_ptr (p_prc, ARATELIA_OGG_DEMUXER_AUDIO_PORT_INDEX);
      *p_port_disabled = true;
      p_port_disabled = get_port_disabled_ptr (p_prc, ARATELIA_OGG_DEMUXER_VIDEO_PORT_INDEX);
      *p_port_disabled = true;
    }
  else
    {
      p_port_disabled = get_port_disabled_ptr (p_prc, a_pid);
      *p_port_disabled = true;
    }

  /* Release any buffers held  */
  TIZ_TRACE (handleOf (p_prc), "port_disable");
  return release_all_buffers (p_prc, a_pid);
}

static OMX_ERRORTYPE
oggdmux_prc_port_enable (const void *ap_obj, OMX_U32 a_pid)
{
  /* TODO */
  return OMX_ErrorNone;
}

/*
 * oggdmux_prc_class
 */

static void *
oggdmux_prc_class_ctor (void *ap_obj, va_list * app)
{
  /* NOTE: Class methods might be added in the future. None for now. */
  return super_ctor (typeOf (ap_obj, "oggdmuxprc_class"), ap_obj, app);
}

/*
 * initialization
 */

void *
oggdmux_prc_class_init (void *ap_tos, void *ap_hdl)
{
  void *tizprc           = tiz_get_type (ap_hdl, "tizprc");
  void *oggdmuxprc_class = factory_new
    /* TIZ_CLASS_COMMENT: class type, class name, parent, size */
    (classOf (tizprc), "oggdmuxprc_class", classOf (tizprc), sizeof (oggdmux_prc_class_t),
     /* TIZ_CLASS_COMMENT: */
     ap_tos, ap_hdl,
     /* TIZ_CLASS_COMMENT: class constructor */
     ctor, oggdmux_prc_class_ctor,
     /* TIZ_CLASS_COMMENT: stop value*/
     0);
  return oggdmuxprc_class;
}

void *
oggdmux_prc_init (void *ap_tos, void *ap_hdl)
{
  void *tizprc = tiz_get_type (ap_hdl, "tizprc");
  void *oggdmuxprc_class = tiz_get_type (ap_hdl, "oggdmuxprc_class");
  void *oggdmuxprc = factory_new
    /* TIZ_CLASS_COMMENT: class type, class name, parent, size */
    (oggdmuxprc_class, "oggdmuxprc", tizprc, sizeof (oggdmux_prc_t),
     /* TIZ_CLASS_COMMENT: */
     ap_tos, ap_hdl,
     /* TIZ_CLASS_COMMENT: class constructor */
     ctor, oggdmux_prc_ctor,
     /* TIZ_CLASS_COMMENT: class destructor */
     dtor, oggdmux_prc_dtor,
     /* TIZ_CLASS_COMMENT: */
     tiz_srv_allocate_resources, oggdmux_prc_allocate_resources,
     /* TIZ_CLASS_COMMENT: */
     tiz_srv_deallocate_resources, oggdmux_prc_deallocate_resources,
     /* TIZ_CLASS_COMMENT: */
     tiz_srv_prepare_to_transfer, oggdmux_prc_prepare_to_transfer,
     /* TIZ_CLASS_COMMENT: */
     tiz_srv_transfer_and_process, oggdmux_prc_transfer_and_process,
     /* TIZ_CLASS_COMMENT: */
     tiz_srv_stop_and_return, oggdmux_prc_stop_and_return,
     /* TIZ_CLASS_COMMENT: */
     tiz_prc_port_flush, oggdmux_prc_port_flush,
     /* TIZ_CLASS_COMMENT: */
     tiz_prc_port_disable, oggdmux_prc_port_disable,
     /* TIZ_CLASS_COMMENT: */
     tiz_prc_port_enable, oggdmux_prc_port_enable,
     /* TIZ_CLASS_COMMENT: */
     tiz_prc_buffers_ready, oggdmux_prc_buffers_ready,
     /* TIZ_CLASS_COMMENT: stop value*/
     0);

  return oggdmuxprc;
}
