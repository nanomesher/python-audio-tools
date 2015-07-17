#include "alac.h"
#include <string.h>
#include <assert.h>
#include <math.h>

/********************************************************
 Audio Tools, a module and set of tools for manipulating audio data
 Copyright (C) 2007-2015  Brian Langenberger

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 2 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*******************************************************/

#define MAX_LPC_ORDER 8
#define INTERLACING_SHIFT 2

static struct alac_frame_size*
push_frame_size(struct alac_frame_size *head,
                unsigned byte_size,
                unsigned pcm_frames_size);

static void
reverse_frame_sizes(struct alac_frame_size **head);

static void
free_alac_frame_sizes(struct alac_frame_size *frame_sizes);

#ifndef STANDALONE

PyObject*
encoders_encode_alac(PyObject *dummy, PyObject *args, PyObject *keywds)
{
    static char *kwlist[] = {"file",
                             "pcmreader",
                             "block_size",
                             "initial_history",
                             "history_multiplier",
                             "maximum_k",
                             NULL};
    PyObject *file_obj;
    BitstreamWriter *output = NULL;
    struct PCMReader *pcmreader;
    int block_size;
    int initial_history;
    int history_multiplier;
    int maximum_k;
    struct alac_frame_size *frame_sizes;

    /*extract a file object, PCMReader-compatible object and encoding options*/
    if (!PyArg_ParseTupleAndKeywords(args, keywds, "OO&iiii",
                                     kwlist,
                                     &file_obj,
                                     py_obj_to_pcmreader,
                                     &pcmreader,
                                     &block_size,
                                     &initial_history,
                                     &history_multiplier,
                                     &maximum_k)) {
        return NULL;
     }

    /*determine if the PCMReader is compatible with ALAC*/
    if ((pcmreader->bits_per_sample != 16) &&
        (pcmreader->bits_per_sample != 24)) {
        PyErr_SetString(PyExc_ValueError, "bits per sample must be 16 or 24");
        return NULL;
    }

    output = bw_open_external(file_obj,
                              BS_BIG_ENDIAN,
                              4096,
                              (ext_write_f)bw_write_python,
                              (ext_setpos_f)bs_setpos_python,
                              (ext_getpos_f)bs_getpos_python,
                              (ext_free_pos_f)bs_free_pos_python,
                              (ext_seek_f)bs_fseek_python,
                              (ext_flush_f)bw_flush_python,
                              (ext_close_f)bs_close_python,
                              (ext_free_f)bs_free_python_nodecref);

    frame_sizes = encode_alac(output,
                              pcmreader,
                              block_size,
                              initial_history,
                              history_multiplier,
                              maximum_k);



    if (frame_sizes) {
        /*convert frame sizes to Python tuple*/
        /*FIXME - should check these calls for errors*/
        PyObject *frame_byte_sizes = PyList_New(0);
        unsigned total_pcm_frames = 0;
        struct alac_frame_size *sizes;

        for (sizes = frame_sizes; sizes; sizes = sizes->next) {
            PyObject *frame_byte_size = Py_BuildValue("I", sizes->byte_size);
            PyList_Append(frame_byte_sizes, frame_byte_size);
            Py_DECREF(frame_byte_size);
            total_pcm_frames += sizes->pcm_frames_size;
        }

        output->flush(output);
        output->free(output);

        free_alac_frame_sizes(frame_sizes);
        return Py_BuildValue("(O,I)", frame_byte_sizes, total_pcm_frames);
    } else {
        /*indicate read error has occurred*/
        output->free(output);

        return NULL;
    }
}

#endif

static struct alac_frame_size*
encode_alac(BitstreamWriter *output,
            struct PCMReader *pcmreader,
            int block_size,
            int initial_history,
            int history_multiplier,
            int maximum_k)
{
    struct alac_context encoder;
    int *samples = malloc(pcmreader->channels *
                          block_size *
                          sizeof(int));
    aa_int* channels = aa_int_new();
    unsigned frame_byte_size = 0;
    bw_pos_t* mdat_header = NULL;
    unsigned pcm_frames_read;
    struct alac_frame_size *frame_sizes = NULL;

    init_encoder(&encoder);

    encoder.options.block_size = block_size;
    encoder.options.initial_history = initial_history;
    encoder.options.history_multiplier = history_multiplier;
    encoder.options.maximum_k = maximum_k;
    encoder.options.minimum_interlacing_leftweight = 0;
    encoder.options.maximum_interlacing_leftweight = 4;

    encoder.bits_per_sample = pcmreader->bits_per_sample;

    /*FIXME - check marks/rewinds for I/O errors*/
    mdat_header = output->getpos(output);

    output->add_callback(output,
                         (bs_callback_f)byte_counter,
                         &frame_byte_size);

    /*write placeholder mdat header*/
    output->write(output, 32, 0);
    output->write_bytes(output, (uint8_t*)"mdat", 4);

    /*write frames from pcm_reader until empty*/
    while ((pcm_frames_read = pcmreader->read(pcmreader,
                                              encoder.options.block_size,
                                              samples)) > 0) {
        const unsigned channel_count = pcmreader->channels;
        unsigned c;

        channels->reset(channels);
        frame_byte_size = 0;

        /*convert flattened channels into lists of samples*/
        for (c = 0; c < channel_count; c++) {
            a_int *channel = channels->append(channels);
            unsigned i;

            channel->resize_for(channel, pcm_frames_read);
            for (i = 0; i < pcm_frames_read; i++) {
                a_append(channel, get_sample(samples, c, channel_count, i));
            }
        }

        /*perform encoding*/
        write_frameset(output, &encoder, channels);

        /*log each frameset's size in bytes and size in samples*/
        frame_sizes = push_frame_size(frame_sizes,
                                      frame_byte_size,
                                      pcm_frames_read);
    }

    output->pop_callback(output, NULL);
    free(samples);
    channels->del(channels);

    if (pcmreader->status == PCM_OK) {
        /*return to header and rewrite it with the actual value*/
        unsigned total_mdat_size = 8;
        struct alac_frame_size *frame_size;

        for (frame_size = frame_sizes;
             frame_size;
             frame_size = frame_size->next) {
            total_mdat_size += frame_size->byte_size;
        }

        output->setpos(output, mdat_header);
        output->write(output, 32, total_mdat_size);
        mdat_header->del(mdat_header);

        /*close and free allocated files/buffers,
          which varies depending on whether we're running standlone or not*/

        free_encoder(&encoder);

        reverse_frame_sizes(&frame_sizes);
        return frame_sizes;
    } else {
        if (mdat_header) {
            mdat_header->del(mdat_header);
        }

        free_encoder(&encoder);

        free_alac_frame_sizes(frame_sizes);
        return NULL;
    }
}

static struct alac_frame_size*
push_frame_size(struct alac_frame_size *head,
                unsigned byte_size,
                unsigned pcm_frames_size)
{
    struct alac_frame_size *frame_size = malloc(sizeof(struct alac_frame_size));
    frame_size->byte_size = byte_size;
    frame_size->pcm_frames_size = pcm_frames_size;
    frame_size->next = head;
    return frame_size;
}

static void
reverse_frame_sizes(struct alac_frame_size **head)
{
    struct alac_frame_size *reversed = NULL;
    struct alac_frame_size *top = *head;
    while (top) {
        *head = (*head)->next;
        top->next = reversed;
        reversed = top;
        top = *head;
    }
    *head = reversed;
}

static void
free_alac_frame_sizes(struct alac_frame_size *frame_sizes)
{
    while (frame_sizes) {
        struct alac_frame_size *next = frame_sizes->next;
        free(frame_sizes);
        frame_sizes = next;
    }
}

static void
init_encoder(struct alac_context* encoder)
{
    encoder->frame_sizes = a_unsigned_new();
    encoder->total_pcm_frames = 0;

    encoder->LSBs = a_int_new();
    encoder->channels_MSB = aa_int_new();

    encoder->correlated_channels = aa_int_new();
    encoder->qlp_coefficients0 = a_int_new();
    encoder->qlp_coefficients1 = a_int_new();
    encoder->residual0 = bw_open_recorder(BS_BIG_ENDIAN);
    encoder->residual1 = bw_open_recorder(BS_BIG_ENDIAN);

    encoder->tukey_window = a_double_new();
    encoder->windowed_signal = a_double_new();
    encoder->autocorrelation_values = a_double_new();
    encoder->lp_coefficients = aa_double_new();
    encoder->qlp_coefficients4 = a_int_new();
    encoder->qlp_coefficients8 = a_int_new();
    encoder->residual_values4 = a_int_new();
    encoder->residual_values8 = a_int_new();
    encoder->residual_block4 = bw_open_recorder(BS_BIG_ENDIAN);
    encoder->residual_block8 = bw_open_recorder(BS_BIG_ENDIAN);

    encoder->compressed_frame = bw_open_recorder(BS_BIG_ENDIAN);
    encoder->interlaced_frame = bw_open_recorder(BS_BIG_ENDIAN);
    encoder->best_interlaced_frame = bw_open_recorder(BS_BIG_ENDIAN);
}

static void
free_encoder(struct alac_context* encoder)
{
    encoder->frame_sizes->del(encoder->frame_sizes);

    encoder->LSBs->del(encoder->LSBs);
    encoder->channels_MSB->del(encoder->channels_MSB);

    encoder->correlated_channels->del(encoder->correlated_channels);
    encoder->qlp_coefficients0->del(encoder->qlp_coefficients0);
    encoder->qlp_coefficients1->del(encoder->qlp_coefficients1);
    encoder->residual0->close(encoder->residual0);
    encoder->residual1->close(encoder->residual1);

    encoder->tukey_window->del(encoder->tukey_window);
    encoder->windowed_signal->del(encoder->windowed_signal);
    encoder->autocorrelation_values->del(encoder->autocorrelation_values);
    encoder->lp_coefficients->del(encoder->lp_coefficients);
    encoder->qlp_coefficients4->del(encoder->qlp_coefficients4);
    encoder->qlp_coefficients8->del(encoder->qlp_coefficients8);
    encoder->residual_values4->del(encoder->residual_values4);
    encoder->residual_values8->del(encoder->residual_values8);
    encoder->residual_block4->close(encoder->residual_block4);
    encoder->residual_block8->close(encoder->residual_block8);

    encoder->compressed_frame->close(encoder->compressed_frame);
    encoder->interlaced_frame->close(encoder->interlaced_frame);
    encoder->best_interlaced_frame->close(encoder->best_interlaced_frame);
}

static inline aa_int*
extract_1ch(aa_int* frameset, unsigned channel, aa_int* pair)
{
    pair->reset(pair);
    frameset->_[channel]->swap(frameset->_[channel],
                               pair->append(pair));
    return pair;
}

static inline aa_int*
extract_2ch(aa_int* frameset, unsigned channel0, unsigned channel1,
            aa_int* pair)
{
    pair->reset(pair);
    frameset->_[channel0]->swap(frameset->_[channel0],
                                pair->append(pair));
    frameset->_[channel1]->swap(frameset->_[channel1],
                                pair->append(pair));
    return pair;
}


static void
write_frameset(BitstreamWriter *bs,
               struct alac_context* encoder,
               aa_int* channels)
{
    aa_int* channel_pair = aa_int_new();
    unsigned i;

    switch (channels->len) {
    case 1:
    case 2:
        write_frame(bs, encoder, channels);
        break;
    case 3:
        write_frame(bs, encoder,
                    extract_1ch(channels, 2, channel_pair));
        write_frame(bs, encoder,
                    extract_2ch(channels, 0, 1, channel_pair));
        break;
    case 4:
        write_frame(bs, encoder,
                    extract_1ch(channels, 2, channel_pair));
        write_frame(bs, encoder,
                    extract_2ch(channels, 0, 1, channel_pair));
        write_frame(bs, encoder,
                    extract_1ch(channels, 3, channel_pair));
        break;
    case 5:
        write_frame(bs, encoder,
                    extract_1ch(channels, 2, channel_pair));
        write_frame(bs, encoder,
                    extract_2ch(channels, 0, 1, channel_pair));
        write_frame(bs, encoder,
                    extract_2ch(channels, 3, 4, channel_pair));
        break;
    case 6:
        write_frame(bs, encoder,
                    extract_1ch(channels, 2, channel_pair));
        write_frame(bs, encoder,
                    extract_2ch(channels, 0, 1, channel_pair));
        write_frame(bs, encoder,
                    extract_2ch(channels, 4, 5, channel_pair));
        write_frame(bs, encoder,
                    extract_1ch(channels, 3, channel_pair));
        break;
    case 7:
        write_frame(bs, encoder,
                    extract_1ch(channels, 2, channel_pair));
        write_frame(bs, encoder,
                    extract_2ch(channels, 0, 1, channel_pair));
        write_frame(bs, encoder,
                    extract_2ch(channels, 4, 5, channel_pair));
        write_frame(bs, encoder,
                    extract_1ch(channels, 6, channel_pair));
        write_frame(bs, encoder,
                    extract_1ch(channels, 3, channel_pair));
        break;
    case 8:
        write_frame(bs, encoder,
                    extract_1ch(channels, 2, channel_pair));
        write_frame(bs, encoder,
                    extract_2ch(channels, 6, 7, channel_pair));
        write_frame(bs, encoder,
                    extract_2ch(channels, 0, 1, channel_pair));
        write_frame(bs, encoder,
                    extract_2ch(channels, 4, 5, channel_pair));
        write_frame(bs, encoder,
                    extract_1ch(channels, 3, channel_pair));
        break;
    default:
        for (i = 0; i < channels->len; i++) {
            write_frame(bs, encoder,
                        extract_1ch(channels, i, channel_pair));
        }
        break;
    }

    bs->write(bs, 3, 7);  /*write the trailing '111' bits*/
    bs->byte_align(bs);   /*and byte-align frameset*/
    channel_pair->del(channel_pair);
}

static void
write_frame(BitstreamWriter *bs,
            struct alac_context* encoder,
            const aa_int* channels)
{
    BitstreamRecorder *compressed_frame;

    assert((channels->len == 1) || (channels->len == 2));

    bs->write(bs, 3, channels->len - 1);

    if ((channels->_[0]->len >= 10)) {
        compressed_frame = encoder->compressed_frame;
        compressed_frame->reset(compressed_frame);
        if (!setjmp(encoder->residual_overflow)) {
            write_compressed_frame((BitstreamWriter*)compressed_frame,
                                   encoder,
                                   channels);
            compressed_frame->copy(compressed_frame, bs);
        } else {
            /*a residual overflow exception occurred,
              so write an uncompressed frame instead*/
            write_uncompressed_frame(bs, encoder, channels);
        }
    } else {
        return write_uncompressed_frame(bs, encoder, channels);
    }
}

static void
write_uncompressed_frame(BitstreamWriter *bs,
                         struct alac_context* encoder,
                         const aa_int* channels)
{
    unsigned i;
    unsigned c;

    bs->write(bs, 16, 0);  /*unused*/

    if (channels->_[0]->len == encoder->options.block_size)
        bs->write(bs, 1, 0);
    else
        bs->write(bs, 1, 1);

    bs->write(bs, 2, 0);  /*no uncompressed LSBs*/
    bs->write(bs, 1, 1);  /*not compressed*/

    if (channels->_[0]->len != encoder->options.block_size)
        bs->write(bs, 32, channels->_[0]->len);

    for (i = 0; i < channels->_[0]->len; i++) {
        for (c = 0; c < channels->len; c++) {
            bs->write_signed(bs,
                             encoder->bits_per_sample,
                             channels->_[c]->_[i]);
        }
    }
}

static void
write_compressed_frame(BitstreamWriter *bs,
                       struct alac_context* encoder,
                       const aa_int* channels)
{
    unsigned uncompressed_LSBs;
    a_int* LSBs;
    aa_int* channels_MSB;
    unsigned i;
    unsigned c;
    unsigned leftweight;
    BitstreamRecorder *interlaced_frame = encoder->interlaced_frame;
    BitstreamRecorder *best_interlaced_frame = encoder->best_interlaced_frame;
    unsigned best_interlaced_frame_bits = UINT_MAX;

    if (encoder->bits_per_sample <= 16) {
        /*no uncompressed least-significant bits*/
        uncompressed_LSBs = 0;
        LSBs = NULL;

        if (channels->len == 1) {
            write_non_interlaced_frame(bs,
                                       encoder,
                                       uncompressed_LSBs,
                                       LSBs,
                                       channels);
        } else {
            /*attempt all the interlacing leftweight combinations*/
            for (leftweight = encoder->options.minimum_interlacing_leftweight;
                 leftweight <= encoder->options.maximum_interlacing_leftweight;
                 leftweight++) {
                interlaced_frame->reset(interlaced_frame);
                write_interlaced_frame((BitstreamWriter*)interlaced_frame,
                                       encoder,
                                       0,
                                       LSBs,
                                       INTERLACING_SHIFT,
                                       leftweight,
                                       channels);
                if (interlaced_frame->bits_written(interlaced_frame) <
                    best_interlaced_frame_bits) {
                    best_interlaced_frame_bits =
                        interlaced_frame->bits_written(interlaced_frame);
                    recorder_swap(&best_interlaced_frame,
                                  &interlaced_frame);
                }
            }

            /*write the smallest leftweight to disk*/
            best_interlaced_frame->copy(best_interlaced_frame, bs);
        }
    } else {
        /*extract uncompressed least-significant bits*/
        uncompressed_LSBs = (encoder->bits_per_sample - 16) / 8;
        LSBs = encoder->LSBs;
        channels_MSB = encoder->channels_MSB;

        LSBs->reset(LSBs);
        channels_MSB->reset(channels_MSB);

        for (c = 0; c < channels->len; c++) {
            channels_MSB->append(channels_MSB);
        }

        for (i = 0; i < channels->_[0]->len; i++) {
            for (c = 0; c < channels->len; c++) {
                LSBs->append(LSBs,
                             channels->_[c]->_[i] &
                             ((1 << (encoder->bits_per_sample - 16)) - 1));
                channels_MSB->_[c]->append(channels_MSB->_[c],
                                              channels->_[c]->_[i] >>
                                              (encoder->bits_per_sample - 16));
            }
        }

        if (channels->len == 1) {
            write_non_interlaced_frame(bs,
                                       encoder,
                                       uncompressed_LSBs,
                                       LSBs,
                                       channels_MSB);
        } else {
            /*attempt all the interlacing leftweight combinations*/

            for (leftweight = encoder->options.minimum_interlacing_leftweight;
                 leftweight <= encoder->options.maximum_interlacing_leftweight;
                 leftweight++) {
                interlaced_frame->reset(interlaced_frame);
                write_interlaced_frame((BitstreamWriter*)interlaced_frame,
                                       encoder,
                                       uncompressed_LSBs,
                                       LSBs,
                                       INTERLACING_SHIFT,
                                       leftweight,
                                       channels_MSB);
                if (interlaced_frame->bits_written(interlaced_frame) <
                    best_interlaced_frame_bits) {
                    best_interlaced_frame_bits =
                        interlaced_frame->bits_written(interlaced_frame);
                    recorder_swap(&best_interlaced_frame,
                                  &interlaced_frame);
                }
            }

            /*write the smallest leftweight to disk*/
            best_interlaced_frame->copy(best_interlaced_frame, bs);
        }
    }
}

static void
write_non_interlaced_frame(BitstreamWriter *bs,
                           struct alac_context* encoder,
                           unsigned uncompressed_LSBs,
                           const a_int* LSBs,
                           const aa_int* channels)
{
    unsigned i;
    a_int* qlp_coefficients = encoder->qlp_coefficients0;
    BitstreamRecorder* residual = encoder->residual0;

    assert(channels->len == 1);
    residual->reset(residual);

    bs->write(bs, 16, 0);  /*unused*/

    if (channels->_[0]->len == encoder->options.block_size)
        bs->write(bs, 1, 0);
    else
        bs->write(bs, 1, 1);

    bs->write(bs, 2, uncompressed_LSBs);
    bs->write(bs, 1, 0);   /*is compressed*/

    if (channels->_[0]->len != encoder->options.block_size)
        bs->write(bs, 32, channels->_[0]->len);

    bs->write(bs, 8, 0);   /*no interlacing shift*/
    bs->write(bs, 8, 0);   /*no interlacing leftweight*/

    compute_coefficients(encoder,
                         channels->_[0],
                         (encoder->bits_per_sample -
                          (uncompressed_LSBs * 8)),
                         qlp_coefficients,
                         (BitstreamWriter*)residual);

    write_subframe_header(bs, qlp_coefficients);

    if (uncompressed_LSBs > 0) {
        for (i = 0; i < LSBs->len; i++) {
            bs->write(bs, uncompressed_LSBs * 8, LSBs->_[i]);
        }
    }

    residual->copy(residual, bs);
}

static void
write_interlaced_frame(BitstreamWriter *bs,
                       struct alac_context* encoder,
                       unsigned uncompressed_LSBs,
                       const a_int* LSBs,
                       unsigned interlacing_shift,
                       unsigned interlacing_leftweight,
                       const aa_int* channels)
{
    unsigned i;
    a_int* qlp_coefficients0 = encoder->qlp_coefficients0;
    BitstreamRecorder* residual0 = encoder->residual0;
    a_int* qlp_coefficients1 = encoder->qlp_coefficients1;
    BitstreamRecorder* residual1 = encoder->residual1;
    aa_int* correlated_channels = encoder->correlated_channels;

    assert(channels->len == 2);
    residual0->reset(residual0);
    residual1->reset(residual1);

    bs->write(bs, 16, 0);  /*unused*/

    if (channels->_[0]->len == encoder->options.block_size)
        bs->write(bs, 1, 0);
    else
        bs->write(bs, 1, 1);

    bs->write(bs, 2, uncompressed_LSBs);
    bs->write(bs, 1, 0);   /*is compressed*/

    if (channels->_[0]->len != encoder->options.block_size)
        bs->write(bs, 32, channels->_[0]->len);

    bs->write(bs, 8, interlacing_shift);
    bs->write(bs, 8, interlacing_leftweight);

    correlate_channels(channels,
                       interlacing_shift,
                       interlacing_leftweight,
                       correlated_channels);

    compute_coefficients(encoder,
                         correlated_channels->_[0],
                         (encoder->bits_per_sample -
                          (uncompressed_LSBs * 8) + 1),
                         qlp_coefficients0,
                         (BitstreamWriter*)residual0);

    compute_coefficients(encoder,
                         correlated_channels->_[1],
                         (encoder->bits_per_sample -
                          (uncompressed_LSBs * 8) + 1),
                         qlp_coefficients1,
                         (BitstreamWriter*)residual1);

    write_subframe_header(bs, qlp_coefficients0);
    write_subframe_header(bs, qlp_coefficients1);

    if (uncompressed_LSBs > 0) {
        for (i = 0; i < LSBs->len; i++) {
            bs->write(bs, uncompressed_LSBs * 8, LSBs->_[i]);
        }
    }

    residual0->copy(residual0, bs);
    residual1->copy(residual1, bs);
}

static void
correlate_channels(const aa_int* channels,
                   unsigned interlacing_shift,
                   unsigned interlacing_leftweight,
                   aa_int* correlated_channels)
{
    a_int* channel0;
    a_int* channel1;
    a_int* correlated0;
    a_int* correlated1;
    unsigned frame_count;

    assert(channels->len == 2);
    assert(channels->_[0]->len == channels->_[1]->len);

    frame_count = channels->_[0]->len;
    channel0 = channels->_[0];
    channel1 = channels->_[1];

    correlated_channels->reset(correlated_channels);
    correlated0 = correlated_channels->append(correlated_channels);
    correlated1 = correlated_channels->append(correlated_channels);
    correlated0->resize(correlated0, frame_count);
    correlated1->resize(correlated1, frame_count);

    if (interlacing_leftweight > 0) {
        unsigned i;

        for (i = 0; i < frame_count; i++) {
            int64_t leftweight = channel0->_[i] - channel1->_[i];
            leftweight *= interlacing_leftweight;
            leftweight >>= interlacing_shift;
            a_append(correlated0, channel1->_[i] + (int)leftweight);
            a_append(correlated1, channel0->_[i] - channel1->_[i]);
        }
    } else {
        channel0->copy(channel0, correlated0);
        channel1->copy(channel1, correlated1);
    }
}

static void
compute_coefficients(struct alac_context* encoder,
                     const a_int* samples,
                     unsigned sample_size,
                     a_int* qlp_coefficients,
                     BitstreamWriter *residual)
{
    a_double* windowed_signal = encoder->windowed_signal;
    a_double* autocorrelation_values = encoder->autocorrelation_values;
    aa_double* lp_coefficients = encoder->lp_coefficients;
    a_int* qlp_coefficients4 = encoder->qlp_coefficients4;
    a_int* qlp_coefficients8 = encoder->qlp_coefficients8;
    a_int* residual_values4 = encoder->residual_values4;
    a_int* residual_values8 = encoder->residual_values8;
    BitstreamRecorder *residual_block4 = encoder->residual_block4;
    BitstreamRecorder *residual_block8 = encoder->residual_block8;

    /*window the input samples*/
    window_signal(encoder,
                  samples,
                  windowed_signal);

    /*compute autocorrelation values for samples*/
    autocorrelate(windowed_signal, autocorrelation_values);

    assert(autocorrelation_values->len == 9);

    if (autocorrelation_values->_[0] != 0.0) {
        /*transform autocorrelation values to lists of LP coefficients*/
        compute_lp_coefficients(autocorrelation_values,
                                lp_coefficients);

        /*quantize LP coefficients at order 4*/
        quantize_coefficients(lp_coefficients, 4, qlp_coefficients4);

        /*quantize LP coefficients at order 8*/
        quantize_coefficients(lp_coefficients, 8, qlp_coefficients8);

        /*calculate residuals for QLP coefficients at order 4*/
        calculate_residuals(samples, sample_size,
                            qlp_coefficients4, residual_values4);

        /*calculate residuals for QLP coefficients at order 8*/
        calculate_residuals(samples, sample_size,
                            qlp_coefficients8, residual_values8);

        /*encode residual block for QLP coefficients at order 4*/
        residual_block4->reset(residual_block4);
        encode_residuals(encoder, sample_size,
                         residual_values4,
                         (BitstreamWriter*)residual_block4);

        /*encode residual block for QLP coefficients at order 8*/
        residual_block8->reset(residual_block8);
        encode_residuals(encoder, sample_size,
                         residual_values8,
                         (BitstreamWriter*)residual_block8);

        /*return the LPC coefficients/residual which is the smallest*/
        if (residual_block4->bits_written(residual_block4) <
            (residual_block8->bits_written(residual_block8) + 64)) {
            /*use QLP coefficients with order 4*/
            qlp_coefficients4->copy(qlp_coefficients4,
                                    qlp_coefficients);
            residual_block4->copy(residual_block4, residual);
        } else {
            /*use QLP coefficients with order 8*/
            qlp_coefficients8->copy(qlp_coefficients8,
                                    qlp_coefficients);
            residual_block8->copy(residual_block8, residual);
        }
    } else {
        /*all samples are 0, so use a special case*/
        qlp_coefficients->mset(qlp_coefficients, 4, 0);

        calculate_residuals(samples, sample_size,
                            qlp_coefficients, residual_values4);

        encode_residuals(encoder, sample_size,
                         residual_values4, residual);
    }
}

static void
window_signal(struct alac_context* encoder,
              const a_int* samples,
              a_double* windowed_signal)
{
    a_double* tukey_window = encoder->tukey_window;
    const unsigned N = samples->len;
    unsigned n;

    if (tukey_window->len != samples->len) {
        const double alpha = 0.5;
        unsigned window1 = (unsigned)(alpha * (N - 1)) / 2;
        unsigned window2 = (unsigned)((N - 1) * (1.0 - (alpha / 2.0)));

        tukey_window->reset_for(tukey_window, samples->len);

        for (n = 0; n < N; n++) {
            if (n <= window1) {
                a_append(tukey_window,
                         0.5 *
                         (1.0 +
                          cos(M_PI * (((2 * n) / (alpha * (N - 1))) - 1.0))));
            } else if (n <= window2) {
                a_append(tukey_window, 1.0);
            } else {
                a_append(tukey_window,
                         0.5 *
                         (1.0 +
                          cos(M_PI * (((2.0 * n) / (alpha * (N - 1))) -
                                      (2.0 / alpha) + 1.0))));
            }
        }
    }

    windowed_signal->reset_for(windowed_signal, samples->len);
    for (n = 0; n < N; n++) {
        a_append(windowed_signal, samples->_[n] * tukey_window->_[n]);
    }
}

static void
autocorrelate(const a_double* windowed_signal,
              a_double* autocorrelation_values)
{
    unsigned lag;

    autocorrelation_values->reset(autocorrelation_values);

    for (lag = 0; lag <= MAX_LPC_ORDER; lag++) {
        double accumulator = 0.0;
        unsigned i;

        assert((windowed_signal->len - lag) > 0);
        for (i = 0; i < windowed_signal->len - lag; i++)
            accumulator += (windowed_signal->_[i] *
                            windowed_signal->_[i + lag]);
        autocorrelation_values->append(autocorrelation_values, accumulator);
    }
}

static void
compute_lp_coefficients(const a_double* autocorrelation_values,
                        aa_double* lp_coefficients)
{
    unsigned i;
    a_double* lp_coeff;
    double k;
    /*no exceptions occur here, so it's okay to allocate temp space*/
    a_double* lp_error = a_double_new();

    assert(autocorrelation_values->len == (MAX_LPC_ORDER + 1));

    lp_coefficients->reset(lp_coefficients);
    lp_error->reset(lp_error);

    k = autocorrelation_values->_[1] / autocorrelation_values->_[0];
    lp_coeff = lp_coefficients->append(lp_coefficients);
    lp_coeff->append(lp_coeff, k);
    lp_error->append(lp_error,
                     autocorrelation_values->_[0] * (1.0 - (k * k)));

    for (i = 1; i < MAX_LPC_ORDER; i++) {
        double q = autocorrelation_values->_[i + 1];
        unsigned j;

        for (j = 0; j < i; j++)
            q -= (lp_coefficients->_[i - 1]->_[j] *
                  autocorrelation_values->_[i - j]);

        k = q / lp_error->_[i - 1];

        lp_coeff = lp_coefficients->append(lp_coefficients);
        for (j = 0; j < i; j++) {
            lp_coeff->append(lp_coeff,
                             lp_coefficients->_[i - 1]->_[j] -
                             (k * lp_coefficients->_[i - 1]->_[i - j - 1]));
        }
        lp_coeff->append(lp_coeff, k);

        lp_error->append(lp_error, lp_error->_[i - 1] * (1.0 - (k * k)));
    }

    lp_error->del(lp_error);
}

static void
quantize_coefficients(const aa_double* lp_coefficients,
                      unsigned order,
                      a_int* qlp_coefficients)
{
    a_double* lp_coeffs = lp_coefficients->_[order - 1];
    const int qlp_max = (1 << 15) - 1;
    const int qlp_min = -(1 << 15);
    double error = 0.0;
    unsigned i;

    assert(lp_coeffs->len == order);

    qlp_coefficients->reset(qlp_coefficients);

    for (i = 0; i < order; i++) {
        error += (lp_coeffs->_[i] * (1 << 9));
        const int error_i = (int)round(error);
        qlp_coefficients->append(qlp_coefficients,
                                 MIN(MAX(error_i, qlp_min), qlp_max));
        error -= (double)error_i;
    }
}

static inline int
SIGN_ONLY(int value)
{
    if (value > 0)
        return 1;
    else if (value < 0)
        return -1;
    else
        return 0;
}

static inline int
TRUNCATE_BITS(int value, unsigned bits)
{
    /*truncate value to bits*/
    const int truncated = value & ((1 << bits) - 1);

    /*apply sign bit*/
    if (truncated & (1 << (bits - 1))) {
        return truncated - (1 << bits);
    } else {
        return truncated;
    }
}

static void
calculate_residuals(const a_int* samples,
                    unsigned sample_size,
                    const a_int* qlp_coefficients,
                    a_int* residuals)
{
    unsigned i = 0;

    /*no exceptions occur here either, so temporary array is safe*/
    a_int* coefficients = a_int_new();
    const unsigned coeff_count = qlp_coefficients->len;

    qlp_coefficients->copy(qlp_coefficients, coefficients);
    residuals->reset_for(residuals, samples->len);

    /*first sample always copied verbatim*/
    a_append(residuals, samples->_[i++]);

    if (coeff_count < 31) {
        unsigned j;

        for (; i < (coeff_count + 1); i++)
            a_append(residuals,
                     TRUNCATE_BITS(samples->_[i] - samples->_[i - 1],
                                   sample_size));

        for (; i < samples->len; i++) {
            const int base_sample = samples->_[i - coeff_count - 1];
            int64_t lpc_sum = 1 << 8;
            int error;

            for (j = 0; j < coeff_count; j++) {
                lpc_sum += ((int64_t)coefficients->_[j] *
                            (int64_t)(samples->_[i - j - 1] - base_sample));
            }

            lpc_sum >>= 9;

            error = TRUNCATE_BITS(samples->_[i] - base_sample - (int)lpc_sum,
                                  sample_size);
            a_append(residuals, error);

            if (error > 0) {
                for (j = 0; j < coeff_count; j++) {
                    const int diff = (base_sample -
                                      samples->_[i - coeff_count + j]);
                    const int sign = SIGN_ONLY(diff);
                    coefficients->_[coeff_count - j - 1] -= sign;
                    error -= ((diff * sign) >> 9) * (j + 1);
                    if (error <= 0)
                        break;
                }
            } else if (error < 0) {
                for (j = 0; j < coeff_count; j++) {
                    const int diff = (base_sample -
                                      samples->_[i - coeff_count + j]);
                    const int sign = SIGN_ONLY(diff);
                    coefficients->_[coeff_count - j - 1] += sign;
                    error -= ((diff * -sign) >> 9) * (j + 1);
                    if (error >= 0)
                        break;
                }
            }
        }
    } else {
        for (; i < samples->len; i++) {
            a_append(residuals,
                     TRUNCATE_BITS(samples->_[i] - samples->_[i - 1],
                                   sample_size));
        }
    }

    coefficients->del(coefficients);
}

static inline unsigned
LOG2(unsigned value)
{
    unsigned bits = 0;
    assert(value > 0);
    while (value) {
        bits++;
        value >>= 1;
    }
    return bits - 1;
}


static void
encode_residuals(struct alac_context* encoder,
                 unsigned sample_size,
                 const a_int* residuals,
                 BitstreamWriter *residual_block)
{
    int history = (int)encoder->options.initial_history;
    unsigned sign_modifier = 0;
    unsigned i = 0;
    unsigned unsigned_i;
    const unsigned max_unsigned = (1 << sample_size);
    const unsigned history_multiplier = encoder->options.history_multiplier;
    const unsigned maximum_k = encoder->options.maximum_k;
    unsigned k;
    unsigned zeroes;

    while (i < residuals->len) {
        if (residuals->_[i] >= 0) {
            unsigned_i = (unsigned)(residuals->_[i] << 1);
        } else {
            unsigned_i = (unsigned)(-residuals->_[i] << 1) - 1;
        }

        if (unsigned_i >= max_unsigned) {
            /*raise a residual overflow exception
              which means writing an uncompressed frame instead*/
            longjmp(encoder->residual_overflow, 1);
        }

        k = LOG2((history >> 9) + 3);
        k = MIN(k, maximum_k);
        write_residual(unsigned_i - sign_modifier, k, sample_size,
                       residual_block);
        sign_modifier = 0;

        if (unsigned_i <= 0xFFFF) {
            history += ((int)(unsigned_i * history_multiplier) -
                        ((history * (int)history_multiplier) >> 9));
            i++;

            if ((history < 128) && (i < residuals->len)) {
                /*handle potential block of 0 residuals*/
                k = 7 - LOG2(history) + ((history + 16) >> 6);
                k = MIN(k, maximum_k);
                zeroes = 0;
                while ((i < residuals->len) && (residuals->_[i] == 0)) {
                    zeroes++;
                    i++;
                }
                write_residual(zeroes, k, 16, residual_block);
                if (zeroes < 0xFFFF)
                    sign_modifier = 1;
                history = 0;
            }
        } else {
            i++;
            history = 0xFFFF;
        }
    }
}

static void
write_residual(unsigned value, unsigned k, unsigned sample_size,
                    BitstreamWriter* residual)
{
    const unsigned MSB = value / ((1 << k) - 1);
    const unsigned LSB = value % ((1 << k) - 1);
    if (MSB > 8) {
        residual->write(residual, 9, 0x1FF);
        residual->write(residual, sample_size, value);
    } else {
        residual->write_unary(residual, 0, MSB);
        if (k > 1) {
            if (LSB > 0) {
                residual->write(residual, k, LSB + 1);
            } else {
                residual->write(residual, k - 1, 0);
            }
        }
    }
}


static void
write_subframe_header(BitstreamWriter *bs,
                      const a_int* qlp_coefficients)
{
    unsigned i;

    bs->write(bs, 4, 0); /*prediction type*/
    bs->write(bs, 4, 9); /*QLP shift needed*/
    bs->write(bs, 3, 4); /*Rice modifier*/
    bs->write(bs, 5, qlp_coefficients->len);
    for (i = 0; i < qlp_coefficients->len; i++) {
        bs->write_signed(bs, 16, qlp_coefficients->_[i]);
    }
}

#ifdef STANDALONE
#include <getopt.h>
#include <errno.h>

static unsigned
count_bits(unsigned value)
{
    unsigned bits = 0;
    while (value) {
        bits += value & 0x1;
        value >>= 1;
    }
    return bits;
}

int main(int argc, char *argv[]) {
    struct PCMReader *pcmreader = NULL;
    char *output_filename = NULL;
    FILE *output_file = NULL;
    BitstreamWriter *output = NULL;
    unsigned channels = 2;
    unsigned channel_mask = 0x3;
    unsigned sample_rate = 44100;
    unsigned bits_per_sample = 16;

    int block_size = 4096;
    int initial_history = 10;
    int history_multiplier = 40;
    int maximum_k = 14;

    struct alac_frame_size *frame_sizes;

    char c;
    const static struct option long_opts[] = {
        {"help",                    no_argument,       NULL, 'h'},
        {"channels",                required_argument, NULL, 'c'},
        {"sample-rate",             required_argument, NULL, 'r'},
        {"bits-per-sample",         required_argument, NULL, 'b'},
        {"block-size",              required_argument, NULL, 'B'},
        {"initial-history",         required_argument, NULL, 'I'},
        {"history-multiplier",      required_argument, NULL, 'M'},
        {"maximum-K",               required_argument, NULL, 'K'},
        {NULL,                      no_argument, NULL, 0}};
    const static char* short_opts = "-hc:r:b:B:M:K:";

    while ((c = getopt_long(argc,
                            argv,
                            short_opts,
                            long_opts,
                            NULL)) != -1) {
        switch (c) {
        case 1:
            if (output_filename == NULL) {
                output_filename = optarg;
            } else {
                printf("only one output file allowed\n");
                return 1;
            }
            break;
        case 'c':
            if (((channels = strtoul(optarg, NULL, 10)) == 0) && errno) {
                printf("invalid --channel \"%s\"\n", optarg);
                return 1;
            }
            break;
        case 'm':
            if (((channel_mask = strtoul(optarg, NULL, 16)) == 0) && errno) {
                printf("invalid --channel-mask \"%s\"\n", optarg);
                return 1;
            }
            break;
        case 'r':
            if (((sample_rate = strtoul(optarg, NULL, 10)) == 0) && errno) {
                printf("invalid --sample-rate \"%s\"\n", optarg);
                return 1;
            }
            break;
        case 'b':
            if (((bits_per_sample = strtoul(optarg, NULL, 10)) == 0) && errno) {
                printf("invalid --bits-per-sample \"%s\"\n", optarg);
                return 1;
            }
            break;
        case 'B':
            if (((block_size = strtol(optarg, NULL, 10)) == 0) && errno) {
                printf("invalid --block-size \"%s\"\n", optarg);
                return 1;
            }
            break;
        case 'I':
            if (((initial_history = strtol(optarg, NULL, 10)) == 0) && errno) {
                printf("invalid --initial-history \"%s\"\n", optarg);
                return 1;
            }
            break;
        case 'M':
            if (((history_multiplier =
                  strtol(optarg, NULL, 10)) == 0) && errno) {
                printf("invalid --history-multiplier \"%s\"\n", optarg);
                return 1;
            }
            break;
        case 'K':
            if (((maximum_k = strtol(optarg, NULL, 10)) == 0) && errno) {
                printf("invalid --maximum-K \"%s\"\n", optarg);
                return 1;
            }
            break;
        case 'h': /*fallthrough*/
        case ':':
        case '?':
            printf("*** Usage: alacenc [options] <output.m4a>\n");
            printf("-c, --channels=#          number of input channels\n");
            printf("-r, --sample_rate=#       input sample rate in Hz\n");
            printf("-b, --bits-per-sample=#   bits per input sample\n");
            printf("\n");
            printf("-B, --block-size=#          block size\n");
            printf("-I, --initial-history=#     initial history\n");
            printf("-M, --history-multiplier=#  history multiplier\n");
            printf("-K, --maximum-K=#           maximum K\n");
            return 0;
        default:
            break;
        }
    }
    if (output_filename) {
        errno = 0;
        if ((output_file = fopen(output_filename, "wb")) == NULL) {
            fprintf(stderr, "%s: %s", output_filename, strerror(errno));
            return 1;
        } else {
            output = bw_open(output_file, BS_BIG_ENDIAN);
        }
    } else {
        fputs("exactly 1 output file required\n", stderr);
        return 1;
    }

    assert(channels > 0);
    assert((bits_per_sample == 8) ||
           (bits_per_sample == 16) ||
           (bits_per_sample == 24));
    assert(sample_rate > 0);
    assert(count_bits(channel_mask) == channels);

    pcmreader = pcmreader_open_raw(stdin,
                                   sample_rate,
                                   channels,
                                   0,
                                   bits_per_sample,
                                   1, 1);

    pcmreader_display(pcmreader, stderr);
    fputs("\n", stderr);
    fprintf(stderr, "block size         %d\n", block_size);
    fprintf(stderr, "initial history    %d\n", initial_history);
    fprintf(stderr, "history multiplier %d\n", history_multiplier);
    fprintf(stderr, "maximum K          %d\n", maximum_k);

    frame_sizes = encode_alac(output,
                              pcmreader,
                              block_size,
                              initial_history,
                              history_multiplier,
                              maximum_k);

    output->close(output);
    pcmreader->close(pcmreader);
    pcmreader->del(pcmreader);

    if (frame_sizes) {
        struct alac_frame_size *sizes;
        for (sizes = frame_sizes; sizes; sizes = sizes->next) {
            fprintf(stderr, "frame size : %u bytes, %u samples\n",
                    sizes->byte_size, sizes->pcm_frames_size);
        }
        free_alac_frame_sizes(frame_sizes);
        return 0;
    } else {
        fputs("*** Error during encoding\n", stderr);
        return 1;
    }
}
#endif
