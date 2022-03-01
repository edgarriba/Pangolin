/* This file is part of the Pangolin Project.
 * http://github.com/stevenlovegrove/Pangolin
 *
 * Copyright (c) 2011 Steven Lovegrove
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

// It is impossible to keep up with ffmpeg deprecations, so ignore these warnings.
#if defined(__GNUC__)
#  pragma GCC diagnostic ignored "-Wdeprecated"
#  pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

#include <array>
#include <pangolin/factory/factory_registry.h>
#include <pangolin/video/iostream_operators.h>
#include <pangolin/video/drivers/ffmpeg.h>
#include <pangolin/utils/file_extension.h>

extern "C"
{
#include <libavformat/avio.h>
#include <libavutil/mathematics.h>
#include <libavdevice/avdevice.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

// Found https://github.com/leandromoreira/ffmpeg-libav-tutorial
// Best reference I've seen for ffmpeg api

namespace pangolin
{

std::string ffmpeg_error_string(int err)
{
    std::string ret(256, '\0');
    av_make_error_string(ret.data(), ret.size(), err);
    return ret;
}


FfmpegVideo::FfmpegVideo(const std::string filename, const std::string strfmtout, const std::string codec_hint, bool dump_info, int user_video_stream, ImageDim size)
    :pFormatCtx(nullptr), pCodecContext(nullptr)
{
    InitUrl(filename, strfmtout, codec_hint, dump_info, user_video_stream, size);
}

void FfmpegVideo::InitUrl(const std::string url, const std::string strfmtout, const std::string codec_hint, bool dump_info, int user_video_stream, ImageDim size)
{
    if( url.find('*') != url.npos )
        throw VideoException("Wildcards not supported. Please use ffmpegs printf style formatting for image sequences. e.g. img-000000%04d.ppm");

    // Register all devices
    avdevice_register_all();

#if (LIBAVFORMAT_VERSION_MAJOR >= 59)
    const AVInputFormat* fmt = nullptr;
#else
    AVInputFormat* fmt = nullptr;
#endif

    if( !codec_hint.empty() ) {
        fmt = av_find_input_format(codec_hint.c_str());
    }

#if (LIBAVFORMAT_VERSION_MAJOR >= 53)
    AVDictionary* options = nullptr;
    if(size.x != 0 && size.y != 0) {
        std::string s = std::to_string(size.x) + "x" + std::to_string(size.y);
        av_dict_set(&options, "video_size", s.c_str(), 0);
    }
    if( avformat_open_input(&pFormatCtx, url.c_str(), fmt, &options) )
#else
    // Deprecated - can't use with mjpeg
    if( av_open_input_file(&pFormatCtx, url.c_str(), fmt, 0, NULL) )
#endif
        throw VideoException("Couldn't open stream");

    if( !ToLowerCopy(codec_hint).compare("mjpeg") )
#ifdef HAVE_FFMPEG_MAX_ANALYZE_DURATION2
        pFormatCtx->max_analyze_duration2 = AV_TIME_BASE * 0.0;
#else
        pFormatCtx->max_analyze_duration = AV_TIME_BASE * 0.0;
#endif

    // Retrieve stream information
#if (LIBAVFORMAT_VERSION_MAJOR >= 53)
    if(avformat_find_stream_info(pFormatCtx, 0)<0)
#else
    // Deprecated
    if(av_find_stream_info(pFormatCtx)<0)
#endif
        throw VideoException("Couldn't find stream information");

    if(dump_info) {
        // Dump information about file onto standard error
#if (LIBAVFORMAT_VERSION_MAJOR >= 53)
        av_dump_format(pFormatCtx, 0, url.c_str(), false);
#else
        // Deprecated
        dump_format(pFormatCtx, 0, url.c_str(), false);
#endif
    }

    const AVCodec *pCodec = nullptr;
    const AVCodecParameters *pCodecParameters =  NULL;
    int found_video_streams = 0;

    // loop though all the streams and print its main information
    for (int i = 0; i < pFormatCtx->nb_streams; i++)
    {
        const AVCodecParameters *pLocalCodecParameters = pFormatCtx->streams[i]->codecpar;

        // finds the registered decoder for a codec ID
        const AVCodec *pLocalCodec = avcodec_find_decoder(pLocalCodecParameters->codec_id);
        if (!pLocalCodec) {
            pango_print_debug("Skipping stream with unsupported codec.");
            continue;
        }

        // When the stream is a video we store its index, codec parameters and codec
        if (pLocalCodecParameters->codec_type == AVMEDIA_TYPE_VIDEO) {
            if (found_video_streams == user_video_stream) {
                pCodec = pLocalCodec;
                pCodecParameters = pLocalCodecParameters;
                videoStream = i;
            }
            ++found_video_streams;
        }
    }

    if(found_video_streams==0 || user_video_stream >= found_video_streams)
        throw VideoException("Couldn't find appropriate video stream");

    packet = av_packet_alloc();
    if (!packet)
        throw VideoException("Failed to allocated memory for AVPacket");

    constexpr bool recompute_index_if_none = true;
    numFrames = pFormatCtx->streams[videoStream]->nb_frames;
    if(!numFrames && recompute_index_if_none)
    {
        // We're going to walk through the whole file without decoding anything
        while(av_read_frame(pFormatCtx, packet) == 0) {
            if(packet->stream_index==videoStream) {
                numFrames++;
            }
            av_packet_unref(packet);
        }
        // Seek back to start
        // ffmpeg will also have buffered seek indices.
        avformat_seek_file(pFormatCtx,videoStream, 0, 0, 0, AVSEEK_FLAG_FRAME | AVSEEK_FLAG_ANY);
    }

    // Find the decoder for the video stream
    pVidCodec = pCodec;
    if(pVidCodec==0)
        throw VideoException("Codec not found");

    // Allocate video frames
#if LIBAVUTIL_VERSION_MAJOR >= 54
    pFrame = av_frame_alloc();
    pFrameOut = av_frame_alloc();
#else
    // deprecated
    pFrame = avcodec_alloc_frame();
    pFrameOut = avcodec_alloc_frame();
#endif
    if(!pFrame || !pFrameOut)
        throw VideoException("Couldn't allocate frames");

    fmtout = FfmpegFmtFromString(strfmtout);
    if(fmtout == AV_PIX_FMT_NONE )
        throw VideoException("Output format not recognised",strfmtout);

    pCodecContext = avcodec_alloc_context3(pCodec);
    if (!pCodecContext)
        throw VideoException("failed to allocated memory for AVCodecContext");

    if (avcodec_parameters_to_context(pCodecContext, pCodecParameters) < 0)
        throw VideoException("failed to copy codec params to codec context");

    if (avcodec_open2(pCodecContext, pCodec, NULL) < 0)
        throw VideoException("failed to open codec through avcodec_open2");


    // Image dimensions
    const int w = pCodecContext->width;
    const int h = pCodecContext->height;

    pFrameOut->width = w;
    pFrameOut->height = h;
    pFrameOut->format = fmtout;
    if(av_frame_get_buffer(pFrameOut, 0) != 0) {
        throw VideoException("");
    }

    // Allocate SWS for converting pixel formats
    img_convert_ctx = sws_getContext(w, h,
                                     pCodecContext->pix_fmt,
                                     w, h, fmtout, SWS_FAST_BILINEAR,
                                     NULL, NULL, NULL);
    if(!img_convert_ctx) {
        throw VideoException("Cannot initialize the conversion context");
    }

    // Populate stream info for users to query
    numBytesOut = 0;
    {
        const PixelFormat strm_fmt = PixelFormatFromString(FfmpegFmtToString(fmtout));
        const size_t pitch = (w*strm_fmt.bpp)/8;
        const size_t size_bytes = h*pitch;
        streams.emplace_back(strm_fmt, w, h, pitch, (unsigned char*)0 + numBytesOut);
        numBytesOut += size_bytes;
    }
}

FfmpegVideo::~FfmpegVideo()
{
    av_free(pFrameOut);
    av_free(pFrame);

    avcodec_close(pCodecContext);

#if (LIBAVFORMAT_VERSION_MAJOR >= 54 || (LIBAVFORMAT_VERSION_MAJOR >= 53 && LIBAVFORMAT_VERSION_MINOR >= 21) )
    avformat_close_input(&pFormatCtx);
#else
    // Deprecated
    av_close_input_file(pFormatCtx);
#endif

    sws_freeContext(img_convert_ctx);
}

const std::vector<StreamInfo>& FfmpegVideo::Streams() const
{
    return streams;
}

size_t FfmpegVideo::SizeBytes() const
{
    return numBytesOut;
}

void FfmpegVideo::Start()
{
}

void FfmpegVideo::Stop()
{
}

bool FfmpegVideo::GrabNext(unsigned char* image, bool /*wait*/)
{
    bool got_packet = false;

    // Read a packet from input until EOF or we get the stream we want
    while(!got_packet) {
        const int r = av_read_frame(pFormatCtx, packet);
        if(r == 0) {
            if(packet->stream_index==videoStream) {
                if(avcodec_send_packet(pCodecContext, packet) == 0) {
                    // codec should now have a frame for us
                    got_packet = true;
                }
            }
            av_packet_unref(packet);
        }else{
            pango_print_info("error av_read_frame. %s\n", ffmpeg_error_string(r).data());
            std::cout << std::flush;
            return false;
        }
    }

    // Packet reading could fail, and the codec could still have a frame for us from the last packet...
    if(avcodec_receive_frame(pCodecContext, pFrame) == 0) {
        sws_scale(img_convert_ctx, pFrame->data, pFrame->linesize, 0, pCodecContext->height, pFrameOut->data, pFrameOut->linesize);
        memcpy(image,pFrameOut->data[0],numBytesOut);
        current_frame++;
        return true;
    }else{
        return false;
    }
}

bool FfmpegVideo::GrabNewest(unsigned char *image, bool wait)
{
    return GrabNext(image,wait);
}

size_t FfmpegVideo::GetCurrentFrameId() const
{
    return current_frame;
}

size_t FfmpegVideo::GetTotalFrames() const
{
    return numFrames;
}

size_t FfmpegVideo::Seek(size_t frameid)
{
    const int res = avformat_seek_file(pFormatCtx,videoStream, frameid, frameid, frameid, AVSEEK_FLAG_FRAME | AVSEEK_FLAG_ANY);
    avcodec_flush_buffers(pCodecContext);

    if(res >= 0) {
        // success
        current_frame = frameid;
        return frameid;
    }else{
        pango_print_info("error whilst seeking. %u, %s\n", (unsigned)frameid, ffmpeg_error_string(res).data());
        return GetCurrentFrameId();
    }
}


PANGOLIN_REGISTER_FACTORY(FfmpegVideo)
{
    struct FfmpegVideoFactory : public TypedFactoryInterface<VideoInterface> {
        std::map<std::string,Precedence> Schemes() const override
        {
            return {{"ffmpeg",0}, {"file",15}, {"files",15}};
        }
        const char* Description() const override
        {
            return "Use the FFMPEG library to decode videos.";
        }
        ParamSet Params() const override
        {
            return {{
                {"fmt","RGB24","Use FFMPEG to decode to this output format."},
                {"stream","0","Decode stream with this index."},
                {"codec_hint","","Apply a hint to FFMPEG on codec. Examples include {MJPEG,video4linux,...}"},
                {"size","","Request a particular size output from FFMPEG"},
                {"verbose","0","Output FFMPEG instantiation information."},
            }};
        }
        std::unique_ptr<VideoInterface> Open(const Uri& uri) override {
            const std::array<std::string,43> ffmpeg_ext = {{
                ".3g2",".3gp", ".amv", ".asf", ".avi", ".drc", ".flv", ".f4v",
                ".f4p", ".f4a", ".f4b", ".gif", ".gifv", ".m4v", ".mkv", ".mng", ".mov", ".qt",
                ".mp4", ".m4p", ".m4v", ".mpg", ".mp2", ".mpeg", ".mpe", ".mpv", ".mpg", ".mpeg",
                ".m2v", ".mxf", ".nsv",  ".ogv", ".ogg", ".rm", ".rmvb", ".roq", ".svi", ".vob",
                ".webm", ".wmv", ".yuv", ".h264", ".h265"
            }};

            if(!uri.scheme.compare("file") || !uri.scheme.compare("files")) {
                const std::string ext = FileLowercaseExtention(uri.url);
                if(std::find(ffmpeg_ext.begin(), ffmpeg_ext.end(), ext) == ffmpeg_ext.end()) {
                    // Don't try to load unknown files without the ffmpeg:// scheme.
                    return std::unique_ptr<VideoInterface>();
                }
            }

            const bool verbose = uri.Get<bool>("verbose",false);
            std::string outfmt = uri.Get<std::string>("fmt","RGB24");
            std::string codec_hint = uri.Get<std::string>("codec_hint","");
            ToUpper(outfmt);
            ToUpper(codec_hint);
            const int video_stream = uri.Get<int>("stream",0);
            const ImageDim size = uri.Get<ImageDim>("size",ImageDim(0,0));
            return std::unique_ptr<VideoInterface>( new FfmpegVideo(uri.url.c_str(), outfmt, codec_hint, verbose, video_stream) );
        }
    };

    return FactoryRegistry::I()->RegisterFactory<VideoInterface>(std::make_shared<FfmpegVideoFactory>());
}

}
