/*****************************************************************************
 * output.c : internal management of output streams for the audio output
 *****************************************************************************
 * Copyright (C) 2002 VideoLAN
 * $Id: output.c,v 1.29 2003/01/20 10:59:29 massiot Exp $
 *
 * Authors: Christophe Massiot <massiot@via.ecp.fr>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/
#include <stdlib.h>                            /* calloc(), malloc(), free() */
#include <string.h>

#include <vlc/vlc.h>

#include "audio_output.h"
#include "aout_internal.h"

/*****************************************************************************
 * aout_OutputNew : allocate a new output and rework the filter pipeline
 *****************************************************************************
 * This function is entered with the mixer lock.
 *****************************************************************************/
int aout_OutputNew( aout_instance_t * p_aout,
                    audio_sample_format_t * p_format )
{
    /* Retrieve user defaults. */
    char * psz_name = config_GetPsz( p_aout, "aout" );
    int i_rate = config_GetInt( p_aout, "aout-rate" );
    vlc_value_t val;

    memcpy( &p_aout->output.output, p_format, sizeof(audio_sample_format_t) );
    if ( i_rate != -1 )
        p_aout->output.output.i_rate = i_rate;
    aout_FormatPrepare( &p_aout->output.output );

    vlc_mutex_lock( &p_aout->output_fifo_lock );

    /* Find the best output plug-in. */
    p_aout->output.p_module = module_Need( p_aout, "audio output",
                                           psz_name );
    if ( psz_name != NULL ) free( psz_name );
    if ( p_aout->output.p_module == NULL )
    {
        msg_Err( p_aout, "no suitable aout module" );
        vlc_mutex_unlock( &p_aout->output_fifo_lock );
        return -1;
    }

    if ( var_Type( p_aout, "audio-channels" ) ==
             (VLC_VAR_STRING | VLC_VAR_HASCHOICE) )
    {
        /* The user may have selected a different channels configuration. */
        var_Get( p_aout, "audio-channels", &val );

        if ( !strcmp( val.psz_string, N_("Both") ) )
        {
            p_aout->output.output.i_original_channels =
                AOUT_CHAN_LEFT | AOUT_CHAN_RIGHT;
        }
        else if ( !strcmp( val.psz_string, N_("Left") ) )
        {
            p_aout->output.output.i_original_channels = AOUT_CHAN_LEFT;
        }
        else if ( !strcmp( val.psz_string, N_("Right") ) )
        {
            p_aout->output.output.i_original_channels = AOUT_CHAN_RIGHT;
        }
        else if ( !strcmp( val.psz_string, N_("Dolby Surround") ) )
        {
            p_aout->output.output.i_original_channels
                = AOUT_CHAN_LEFT | AOUT_CHAN_RIGHT | AOUT_CHAN_DOLBYSTEREO;
        }
        free( val.psz_string );
    }
    else if ( p_aout->output.output.i_physical_channels == AOUT_CHAN_CENTER
              && (p_aout->output.output.i_original_channels
                   & AOUT_CHAN_PHYSMASK) == (AOUT_CHAN_LEFT | AOUT_CHAN_RIGHT) )
    {
        /* Mono - create the audio-channels variable. */
        var_Create( p_aout, "audio-channels", VLC_VAR_STRING | VLC_VAR_HASCHOICE );
        if ( p_aout->output.output.i_original_channels & AOUT_CHAN_DUALMONO )
        {
            /* Go directly to the left channel. */
            p_aout->output.output.i_original_channels = AOUT_CHAN_LEFT;
        }
        else
        {
            val.psz_string = N_("Both");
            var_Change( p_aout, "audio-channels", VLC_VAR_ADDCHOICE, &val );
        }
        val.psz_string = N_("Left");
        var_Change( p_aout, "audio-channels", VLC_VAR_ADDCHOICE, &val );
        val.psz_string = N_("Right");
        var_Change( p_aout, "audio-channels", VLC_VAR_ADDCHOICE, &val );
        var_AddCallback( p_aout, "audio-channels", aout_ChannelsRestart,
                         NULL );
    }
    else if ( p_aout->output.output.i_physical_channels ==
                 (AOUT_CHAN_LEFT | AOUT_CHAN_RIGHT)
              && (p_aout->output.output.i_original_channels
                   & AOUT_CHAN_PHYSMASK) == (AOUT_CHAN_LEFT | AOUT_CHAN_RIGHT) )
    {
        /* Stereo - create the audio-channels variable. */
        var_Create( p_aout, "audio-channels", VLC_VAR_STRING | VLC_VAR_HASCHOICE );
        val.psz_string = N_("Both");
        var_Change( p_aout, "audio-channels", VLC_VAR_ADDCHOICE, &val );
        val.psz_string = N_("Left");
        var_Change( p_aout, "audio-channels", VLC_VAR_ADDCHOICE, &val );
        val.psz_string = N_("Right");
        var_Change( p_aout, "audio-channels", VLC_VAR_ADDCHOICE, &val );
        if ( p_aout->output.output.i_original_channels & AOUT_CHAN_DOLBYSTEREO )
        {
            val.psz_string = N_("Dolby Surround");
            var_Change( p_aout, "audio-channels", VLC_VAR_ADDCHOICE, &val );
        }
        p_aout->output.output.i_original_channels &= ~AOUT_CHAN_DOLBYSTEREO;
        var_AddCallback( p_aout, "audio-channels", aout_ChannelsRestart,
                         NULL );
    }
    val.b_bool = VLC_TRUE;
    var_Set( p_aout, "intf-change", val );

    aout_FormatPrepare( &p_aout->output.output );

    /* Prepare FIFO. */
    aout_FifoInit( p_aout, &p_aout->output.fifo,
                   p_aout->output.output.i_rate );

    vlc_mutex_unlock( &p_aout->output_fifo_lock );

    aout_FormatPrint( p_aout, "output", &p_aout->output.output );

    /* Calculate the resulting mixer output format. */
    memcpy( &p_aout->mixer.mixer, &p_aout->output.output,
            sizeof(audio_sample_format_t) );
    if ( !AOUT_FMT_NON_LINEAR(&p_aout->output.output) )
    {
        /* Non-S/PDIF mixer only deals with float32 or fixed32. */
        p_aout->mixer.mixer.i_format
                     = (p_aout->p_libvlc->i_cpu & CPU_CAPABILITY_FPU) ?
                        VLC_FOURCC('f','l','3','2') :
                        VLC_FOURCC('f','i','3','2');
        aout_FormatPrepare( &p_aout->mixer.mixer );
    }
    else
    {
        p_aout->mixer.mixer.i_format = p_format->i_format;
    }

    aout_FormatPrint( p_aout, "mixer", &p_aout->output.output );

    /* Create filters. */
    if ( aout_FiltersCreatePipeline( p_aout, p_aout->output.pp_filters,
                                     &p_aout->output.i_nb_filters,
                                     &p_aout->mixer.mixer,
                                     &p_aout->output.output ) < 0 )
    {
        msg_Err( p_aout, "couldn't set an output pipeline" );
        module_Unneed( p_aout, p_aout->output.p_module );
        return -1;
    }

    /* Prepare hints for the buffer allocator. */
    p_aout->mixer.output_alloc.i_alloc_type = AOUT_ALLOC_HEAP;
    p_aout->mixer.output_alloc.i_bytes_per_sec
                        = p_aout->mixer.mixer.i_bytes_per_frame
                           * p_aout->mixer.mixer.i_rate
                           / p_aout->mixer.mixer.i_frame_length;

    aout_FiltersHintBuffers( p_aout, p_aout->output.pp_filters,
                             p_aout->output.i_nb_filters,
                             &p_aout->mixer.output_alloc );

    return 0;
}

/*****************************************************************************
 * aout_OutputDelete : delete the output
 *****************************************************************************
 * This function is entered with the mixer lock.
 *****************************************************************************/
void aout_OutputDelete( aout_instance_t * p_aout )
{
    module_Unneed( p_aout, p_aout->output.p_module );

    aout_FiltersDestroyPipeline( p_aout, p_aout->output.pp_filters,
                                 p_aout->output.i_nb_filters );
    aout_FifoDestroy( p_aout, &p_aout->output.fifo );
}

/*****************************************************************************
 * aout_OutputPlay : play a buffer
 *****************************************************************************
 * This function is entered with the mixer lock.
 *****************************************************************************/
void aout_OutputPlay( aout_instance_t * p_aout, aout_buffer_t * p_buffer )
{
    aout_FiltersPlay( p_aout, p_aout->output.pp_filters,
                      p_aout->output.i_nb_filters,
                      &p_buffer );

    vlc_mutex_lock( &p_aout->output_fifo_lock );
    aout_FifoPush( p_aout, &p_aout->output.fifo, p_buffer );
    p_aout->output.pf_play( p_aout );
    vlc_mutex_unlock( &p_aout->output_fifo_lock );
}

/*****************************************************************************
 * aout_OutputNextBuffer : give the audio output plug-in the right buffer
 *****************************************************************************
 * If b_can_sleek is 1, the aout core functions won't try to resample
 * new buffers to catch up - that is we suppose that the output plug-in can
 * compensate it by itself. S/PDIF outputs should always set b_can_sleek = 1.
 * This function is entered with no lock at all :-).
 *****************************************************************************/
aout_buffer_t * aout_OutputNextBuffer( aout_instance_t * p_aout,
                                       mtime_t start_date,
                                       vlc_bool_t b_can_sleek )
{
    aout_buffer_t * p_buffer;

    vlc_mutex_lock( &p_aout->output_fifo_lock );

    p_buffer = p_aout->output.fifo.p_first;
    while ( p_buffer && p_buffer->start_date < mdate() )
    {
        msg_Dbg( p_aout, "audio output is too slow ("I64Fd"), "
                 "trashing "I64Fd"us", mdate() - p_buffer->start_date,
                 p_buffer->end_date - p_buffer->start_date );
        p_buffer = p_buffer->p_next;
        aout_BufferFree( p_aout->output.fifo.p_first );
        p_aout->output.fifo.p_first = p_buffer;
    }

    if ( p_buffer == NULL )
    {
        p_aout->output.fifo.pp_last = &p_aout->output.fifo.p_first;

#if 0 /* This is bad because the audio output might just be trying to fill
       * in it's internal buffers. And anyway, it's up to the audio output
       * to deal with this kind of starvation. */

        /* Set date to 0, to allow the mixer to send a new buffer ASAP */
        aout_FifoSet( p_aout, &p_aout->output.fifo, 0 );
        if ( !p_aout->output.b_starving )
            msg_Dbg( p_aout,
                 "audio output is starving (no input), playing silence" );
        p_aout->output.b_starving = 1;
#endif

        vlc_mutex_unlock( &p_aout->output_fifo_lock );
        return NULL;
    }

    /* Here we suppose that all buffers have the same duration - this is
     * generally true, and anyway if it's wrong it won't be a disaster. */
    if ( p_buffer->start_date > start_date
                         + (p_buffer->end_date - p_buffer->start_date) )
    {
        vlc_mutex_unlock( &p_aout->output_fifo_lock );
        if ( !p_aout->output.b_starving )
            msg_Dbg( p_aout, "audio output is starving ("I64Fd"), "
                     "playing silence", p_buffer->start_date - start_date );
        p_aout->output.b_starving = 1;
        return NULL;
    }

    p_aout->output.b_starving = 0;

    if ( !b_can_sleek &&
          ( (p_buffer->start_date - start_date > AOUT_PTS_TOLERANCE)
             || (start_date - p_buffer->start_date > AOUT_PTS_TOLERANCE) ) )
    {
        /* Try to compensate the drift by doing some resampling. */
        int i;
        mtime_t difference = start_date - p_buffer->start_date;
        msg_Warn( p_aout, "output date isn't PTS date, requesting "
                  "resampling ("I64Fd")", difference );

        vlc_mutex_lock( &p_aout->input_fifos_lock );
        for ( i = 0; i < p_aout->i_nb_inputs; i++ )
        {
            aout_fifo_t * p_fifo = &p_aout->pp_inputs[i]->fifo;

            aout_FifoMoveDates( p_aout, p_fifo, difference );
        }

        aout_FifoMoveDates( p_aout, &p_aout->output.fifo, difference );
        vlc_mutex_unlock( &p_aout->input_fifos_lock );
    }

    p_aout->output.fifo.p_first = p_buffer->p_next;
    if ( p_buffer->p_next == NULL )
    {
        p_aout->output.fifo.pp_last = &p_aout->output.fifo.p_first;
    }

    vlc_mutex_unlock( &p_aout->output_fifo_lock );
    return p_buffer;
}
