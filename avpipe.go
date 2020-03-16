/*
Package avpipe has four main interfaces that has to be implemented by the client code:

  1) InputOpener: is the input factory interface that needs an implementation to generate an InputHandler.

  2) InputHandler: is the input handler with Read/Seek/Size/Close methods. An implementation of this
     interface is needed by ffmpeg to process input streams properly.

  3) OutputOpener: is the output factory interface that needs an implementation to generate an OutputHandler.

  4) OutputHandler: is the output handler with Write/Seek/Close methods. An implementation of this
     interface is needed by ffmpeg to write encoded streams properly.

*/
package avpipe

// #cgo pkg-config: libavcodec libavfilter libavformat libavutil libswresample libavresample
// #cgo CFLAGS: -I./include
// #include <string.h>
// #include <stdlib.h>
// #include "avpipe_xc.h"
// #include "avpipe.h"
// #include "elv_log.h"
import "C"
import (
	"fmt"
	"math/big"
	"sync"
	"unsafe"

	elog "github.com/qluvio/content-fabric/log"
)

var log = elog.Get("/eluvio/avpipe")

const traceIo bool = false

// AVType ...
type AVType int

const (
	// Unknown 0
	Unknown AVType = iota
	// DASHManifest 1
	DASHManifest
	// DASHVideoInit 2
	DASHVideoInit
	// DASHVideoSegment 3
	DASHVideoSegment
	// DASHAudioInit 4
	DASHAudioInit
	// DASHAudioSegment 5
	DASHAudioSegment
	// HLSMasterM3U 6
	HLSMasterM3U
	// HLSVideoM3U 7
	HLSVideoM3U
	// HLSAudioM3U 8
	HLSAudioM3U
	// AES128Key 9
	AES128Key
	// MP4Stream 10
	MP4Stream
	// FMP4Stream 11 (Fragmented MP4)
	FMP4Stream
	// MP4Segment 12
	MP4Segment
	// FMP4Segment 13
	FMP4Segment
)

type TxType int

const (
	TxNone TxType = iota
	TxVideo
	TxAudio
	TxAll
)

// CryptScheme is the content encryption scheme
type CryptScheme int

const (
	// CryptNone - clear
	CryptNone CryptScheme = iota
	// CryptAES128 - AES-128
	CryptAES128
	// CryptCENC - CENC AES-CTR
	CryptCENC
	// CryptCBC1 - CENC AES-CBC
	CryptCBC1
	// CryptCENS - CENC AES-CTR Pattern
	CryptCENS
	// CryptCBCS - CENC AES-CBC Pattern
	CryptCBCS
)

// TxParams should match with txparams_t in avpipe_xc.h
type TxParams struct {
	BypassTranscoding     bool        `json:"bypass,omitempty"`
	Format                string      `json:"format,omitempty"`
	StartTimeTs           int64       `json:"start_time_ts,omitempty"`
	SkipOverPts           int64       `json:"skip_over_pts,omitempty"`
	StartPts              int64       `json:"start_pts,omitempty"` // Start PTS for output
	DurationTs            int64       `json:"duration_ts,omitempty"`
	StartSegmentStr       string      `json:"start_segment_str,omitempty"`
	VideoBitrate          int32       `json:"video_bitrate,omitempty"`
	AudioBitrate          int32       `json:"audio_bitrate,omitempty"`
	SampleRate            int32       `json:"sample_rate,omitempty"` // Audio sampling rate
	RcMaxRate             int32       `json:"rc_max_rate,omitempty"`
	RcBufferSize          int32       `json:"rc_buffer_size,omitempty"`
	CrfStr                string      `json:"crf_str,omitempty"`
	SegDurationTs         int64       `json:"seg_duration_ts,omitempty"`
	SegDuration           string      `json:"seg_duration,omitempty"`
	StartFragmentIndex    int32       `json:"start_fragment_index,omitempty"`
	ForceKeyInt           int32       `json:"force_keyint,omitempty"`
	Ecodec                string      `json:"ecodec,omitempty"` // Video encoder
	Dcodec                string      `json:"dcodec,omitempty"` // Video decoder
	EncHeight             int32       `json:"enc_height,omitempty"`
	EncWidth              int32       `json:"enc_width,omitempty"`
	CryptIV               string      `json:"crypt_iv,omitempty"`
	CryptKey              string      `json:"crypt_key,omitempty"`
	CryptKID              string      `json:"crypt_kid,omitempty"`
	CryptKeyURL           string      `json:"crypt_key_url,omitempty"`
	CryptScheme           CryptScheme `json:"crypt_scheme,omitempty"`
	TxType                TxType      `json:"tx_type,omitempty"`
	Seekable              bool        `json:"seekable,omitempty"`
	WatermarkText         string      `json:"watermark_text,omitempty"`
	WatermarkXLoc         string      `json:"watermark_xloc,omitempty"`
	WatermarkYLoc         string      `json:"watermark_yloc,omitempty"`
	WatermarkRelativeSize float32     `json:"watermark_relative_size,omitempty"`
	WatermarkFontColor    string      `json:"watermark_font_color,omitempty"`
	WatermarkShadow       bool        `json:"watermark_shadow,omitempty"`
	WatermarkShadowColor  string      `json:"watermark_shadow_color,omitempty"`
	AudioIndex            int32       `json:"audio_index,omitempty"`
}

type AVMediaType int

const (
	AVMEDIA_TYPE_UNKNOWN    = -1
	AVMEDIA_TYPE_VIDEO      = 0
	AVMEDIA_TYPE_AUDIO      = 1
	AVMEDIA_TYPE_DATA       = 2 ///< Opaque data information usually continuous
	AVMEDIA_TYPE_SUBTITLE   = 3
	AVMEDIA_TYPE_ATTACHMENT = 4 ///< Opaque data information usually sparse
	AVMEDIA_TYPE_NB         = 5
)

var AVMediaTypeNames = map[AVMediaType]string{
	AVMEDIA_TYPE_UNKNOWN:    "unknown",
	AVMEDIA_TYPE_VIDEO:      "video",
	AVMEDIA_TYPE_AUDIO:      "audio",
	AVMEDIA_TYPE_DATA:       "data",
	AVMEDIA_TYPE_SUBTITLE:   "subtitle",
	AVMEDIA_TYPE_ATTACHMENT: "attachment",
	AVMEDIA_TYPE_NB:         "nb",
}

type AVFieldOrder int

const (
	AV_FIELD_UNKNOWN     = 0
	AV_FIELD_PROGRESSIVE = 1
	AV_FIELD_TT          = 2 //< Top coded_first, top displayed first
	AV_FIELD_BB          = 3 //< Bottom coded first, bottom displayed first
	AV_FIELD_TB          = 4 //< Top coded first, bottom displayed first
	AV_FIELD_BT          = 5 //< Bottom coded first, top displayed first
)

var AVFieldOrderNames = map[AVFieldOrder]string{
	AV_FIELD_UNKNOWN:     "",
	AV_FIELD_PROGRESSIVE: "progressive",
	AV_FIELD_TT:          "tt",
	AV_FIELD_BB:          "bb",
	AV_FIELD_TB:          "tb",
	AV_FIELD_BT:          "bt",
}

type AVStatType int

const (
	AV_IN_STAT_BYTES_READ          = 1
	AV_OUT_STAT_BYTES_WRITTEN      = 2
	AV_OUT_STAT_DECODING_START_PTS = 3
	AV_OUT_STAT_ENCODING_END_PTS   = 4
)

type StreamInfo struct {
	StreamIndex        int      `json:"stream_index"`
	CodecType          string   `json:"codec_type"`
	CodecID            int      `json:"codec_id,omitempty"`
	CodecName          string   `json:"codec_name,omitempty"`
	DurationTs         int64    `json:"duration_ts,omitempty"`
	TimeBase           *big.Rat `json:"time_base,omitempty"`
	NBFrames           int64    `json:"nb_frames,omitempty"`
	StartTime          int64    `json:"start_time"` // in TS unit
	AvgFrameRate       *big.Rat `json:"avg_frame_rate,omitempty"`
	FrameRate          *big.Rat `json:"frame_rate,omitempty"`
	SampleRate         int      `json:"sample_rate,omitempty"`
	Channels           int      `json:"channels,omitempty"`
	ChannelLayout      int      `json:"channel_layout,omitempty"`
	TicksPerFrame      int      `json:"ticks_per_frame,omitempty"`
	BitRate            int64    `json:"bit_rate,omitempty"`
	Has_B_Frames       bool     `json:"has_b_frame"`
	Width              int      `json:"width,omitempty"`  // Video only
	Height             int      `json:"height,omitempty"` // Video only
	PixFmt             int      `json:"pix_fmt"`          // Video only, it matches with enum AVPixelFormat in FFmpeg
	SampleAspectRatio  *big.Rat `json:"sample_aspect_ratio,omitempty"`
	DisplayAspectRatio *big.Rat `json:"display_aspect_ratio,omitempty"`
	FieldOrder         string   `json:"field_order,omitempty"`
	Profile            int      `json:"profile,omitempty"`
	Level              int      `json:"level,omitempty"`
}

type ContainerInfo struct {
	Duration   float64 `json:"duration"`
	FormatName string  `json:"format_name"`
}

// PENDING: use legacy_imf_dash_extract/media.Probe?
type ProbeInfo struct {
	ContainerInfo ContainerInfo `json:"format"`
	StreamInfo    []StreamInfo  `json:"streams"`
}

// IOHandler defines handlers that will be called from the C interface functions
type IOHandler interface {
	InReader(buf []byte) (int, error)
	InSeeker(offset C.int64_t, whence C.int) error
	InCloser() error
	InStat(avp_stat C.avp_stat_t, stat_args *C.void) error
	OutWriter(fd C.int, buf []byte) (int, error)
	OutSeeker(fd C.int, offset C.int64_t, whence C.int) (int64, error)
	OutCloser(fd C.int) error
	OutStat(avp_stat C.avp_stat_t, stat_args *C.void) error
}

type InputOpener interface {
	// fd determines uniquely opening input.
	// url determines input string for transcoding
	Open(fd int64, url string) (InputHandler, error)
}

type InputHandler interface {
	// Reads from input stream into buf.
	// Returns (0, nil) to indicate EOF.
	Read(buf []byte) (int, error)

	// Seeks to specific offset of the input.
	Seek(offset int64, whence int) (int64, error)

	// Closes the input.
	Close() error

	// Returns the size of input, if the size is not known returns 0 or -1.
	Size() int64

	// Reports some stats
	Stat(statType AVStatType, statArgs interface{}) error
}

type OutputOpener interface {
	// h determines uniquely opening input.
	// fd determines uniquely opening output.
	Open(h, fd int64, stream_index, seg_index int, out_type AVType) (OutputHandler, error)
}

type OutputHandler interface {
	// Writes encoded stream to the output.
	Write(buf []byte) (int, error)

	// Seeks to specific offset of the output.
	Seek(offset int64, whence int) (int64, error)

	// Closes the output.
	Close() error

	// Reports some stats
	Stat(statType AVStatType, statArgs interface{}) error
}

// Implement IOHandler
type ioHandler struct {
	input    InputHandler // Input file
	mutex    *sync.Mutex
	outTable map[int64]OutputHandler // Map of integer handle to output interfaces
}

// Global table of handlers
var gHandlers map[int64]*ioHandler = make(map[int64]*ioHandler)
var gURLInputOpeners map[string]InputOpener = make(map[string]InputOpener)    // Keeps InputOpener for specific URL
var gURLOutputOpeners map[string]OutputOpener = make(map[string]OutputOpener) // Keeps OutputOpener for specific URL
var gHandleNum int64
var gFd int64
var gMutex sync.Mutex
var gInputOpener InputOpener
var gOutputOpener OutputOpener

// This is used to set global input/output opener for avpipe
// If there is no specific input/output opener for a URL, the global
// input/output opener will be used.
func InitIOHandler(inputOpener InputOpener, outputOpener OutputOpener) {
	gInputOpener = inputOpener
	gOutputOpener = outputOpener
}

// This is used to set input/output opener specific to a URL.
// The input/output opener set by this function, is only valid for the URL and will be unset after
// Tx() or Probe() is complete.
func InitUrlIOHandler(url string, inputOpener InputOpener, outputOpener OutputOpener) {
	if inputOpener != nil {
		gMutex.Lock()
		gURLInputOpeners[url] = inputOpener
		gMutex.Unlock()
	}

	if outputOpener != nil {
		gMutex.Lock()
		gURLOutputOpeners[url] = outputOpener
		gMutex.Unlock()
	}
}

//export NewIOHandler
func NewIOHandler(url *C.char, size *C.int64_t) C.int64_t {
	filename := C.GoString((*C.char)(unsafe.Pointer(url)))
	urlInputOpener, ok := gURLInputOpeners[filename]
	if !ok {
		urlInputOpener = gInputOpener
	}

	urlOutputOpener, ok := gURLOutputOpeners[filename]
	if !ok {
		urlOutputOpener = gOutputOpener
	}

	if urlInputOpener == nil || urlOutputOpener == nil {
		log.Error("Input or output opener(s) are not set")
		return C.int64_t(-1)
	}
	log.Debug("NewIOHandler()", "url", filename)

	gMutex.Lock()
	gHandleNum++
	fd := gHandleNum
	gMutex.Unlock()

	input, err := urlInputOpener.Open(fd, filename)
	if err != nil {
		return C.int64_t(-1)
	}

	*size = C.int64_t(input.Size())

	h := &ioHandler{input: input, outTable: make(map[int64]OutputHandler), mutex: &sync.Mutex{}}
	log.Debug("NewIOHandler()", "url", filename, "size", *size)

	gMutex.Lock()
	defer gMutex.Unlock()
	gHandlers[fd] = h
	return C.int64_t(fd)
}

//export AVPipeReadInput
func AVPipeReadInput(fd C.int64_t, buf *C.uint8_t, sz C.int) C.int {
	gMutex.Lock()
	h := gHandlers[int64(fd)]
	if h == nil {
		gMutex.Unlock()
		return C.int(-1)
	}
	gMutex.Unlock()

	if traceIo {
		log.Debug("AVPipeReadInput()", "fd", fd, "buf", buf, "sz", sz)
	}

	//gobuf := C.GoBytes(unsafe.Pointer(buf), sz)
	gobuf := make([]byte, sz)

	n, err := h.InReader(gobuf)
	if n > 0 {
		C.memcpy(unsafe.Pointer(buf), unsafe.Pointer(&gobuf[0]), C.size_t(n))
	}

	if err != nil {
		return C.int(-1)
	}

	return C.int(n) // PENDING err
}

func (h *ioHandler) InReader(buf []byte) (int, error) {
	n, err := h.input.Read(buf)

	if traceIo {
		log.Debug("InReader()", "buf_size", len(buf), "n", n, "error", err)
	}
	return n, err
}

//export AVPipeSeekInput
func AVPipeSeekInput(fd C.int64_t, offset C.int64_t, whence C.int) C.int64_t {
	gMutex.Lock()
	h := gHandlers[int64(fd)]
	if h == nil {
		gMutex.Unlock()
		return C.int64_t(-1)
	}
	gMutex.Unlock()
	if traceIo {
		log.Debug("AVPipeSeekInput()", "h", h)
	}

	n, err := h.InSeeker(offset, whence)
	if err != nil {
		return C.int64_t(-1)
	}
	return C.int64_t(n)
}

func (h *ioHandler) InSeeker(offset C.int64_t, whence C.int) (int64, error) {
	n, err := h.input.Seek(int64(offset), int(whence))
	log.Debug("InSeeker()", "offset", offset, "whence", whence, "n", n)
	return n, err
}

//export AVPipeCloseInput
func AVPipeCloseInput(fd C.int64_t) C.int {
	gMutex.Lock()
	h := gHandlers[int64(fd)]
	if h == nil {
		gMutex.Unlock()
		return C.int(-1)
	}
	err := h.InCloser()

	// Remove the handler from global table
	gHandlers[int64(fd)] = nil
	gMutex.Unlock()
	if err != nil {
		return C.int(-1)
	}

	return C.int(0)
}

func (h *ioHandler) InCloser() error {
	err := h.input.Close()
	log.Debug("InCloser()", "error", err)
	return err
}

//export AVPipeStatInput
func AVPipeStatInput(fd C.int64_t, avp_stat C.avp_stat_t, stat_args unsafe.Pointer) C.int {
	gMutex.Lock()
	h := gHandlers[int64(fd)]
	if h == nil {
		gMutex.Unlock()
		return C.int(-1)
	}
	gMutex.Unlock()

	err := h.InStat(avp_stat, stat_args)
	if err != nil {
		return C.int(-1)
	}

	return C.int(0)
}

func (h *ioHandler) InStat(avp_stat C.avp_stat_t, stat_args unsafe.Pointer) error {
	var err error

	switch avp_stat {
	case C.in_stat_bytes_read:
		statArgs := *(*uint64)(stat_args)
		err = h.input.Stat(AV_IN_STAT_BYTES_READ, &statArgs)
	}

	return err
}

func (h *ioHandler) putOutTable(fd int64, outHandler OutputHandler) {
	h.mutex.Lock()
	defer h.mutex.Unlock()

	h.outTable[fd] = outHandler
}

func (h *ioHandler) getOutTable(fd int64) OutputHandler {
	h.mutex.Lock()
	defer h.mutex.Unlock()

	return h.outTable[fd]
}

//export AVPipeOpenOutput
func AVPipeOpenOutput(handler C.int64_t, stream_index, seg_index, stream_type C.int) C.int64_t {
	var out_type AVType

	gMutex.Lock()
	h := gHandlers[int64(handler)]
	if h == nil {
		gMutex.Unlock()
		return C.int64_t(-1)
	}
	gFd++
	fd := gFd
	gMutex.Unlock()
	switch stream_type {
	case C.avpipe_video_init_stream:
		out_type = DASHVideoInit
	case C.avpipe_audio_init_stream:
		out_type = DASHAudioInit
	case C.avpipe_manifest:
		out_type = DASHManifest
	case C.avpipe_video_segment:
		out_type = DASHVideoSegment
	case C.avpipe_audio_segment:
		out_type = DASHAudioSegment
	case C.avpipe_master_m3u:
		out_type = HLSMasterM3U
	case C.avpipe_video_m3u:
		out_type = HLSVideoM3U
	case C.avpipe_audio_m3u:
		out_type = HLSAudioM3U
	case C.avpipe_aes_128_key:
		out_type = AES128Key
	case C.avpipe_mp4_stream:
		out_type = MP4Stream
	case C.avpipe_fmp4_stream:
		out_type = FMP4Stream
	case C.avpipe_mp4_segment:
		out_type = MP4Segment
	case C.avpipe_fmp4_segment:
		out_type = FMP4Segment
	default:
		log.Error("AVPipeOpenOutput()", "invalid stream type", stream_type)
		return C.int64_t(-1)
	}

	outHandler, err := gOutputOpener.Open(int64(handler), fd, int(stream_index), int(seg_index), out_type)
	if err != nil {
		log.Error("AVPipeOpenOutput()", "out_type", out_type, "error", err)
		return C.int64_t(-1)
	}

	log.Debug("AVPipeOpenOutput()", "fd", fd, "stream_index", stream_index, "seg_index", seg_index, "out_type", out_type)
	h.putOutTable(fd, outHandler)

	return C.int64_t(fd)
}

//export AVPipeWriteOutput
func AVPipeWriteOutput(handler C.int64_t, fd C.int64_t, buf *C.uint8_t, sz C.int) C.int {
	gMutex.Lock()
	h := gHandlers[int64(handler)]
	if h == nil {
		gMutex.Unlock()
		return C.int(-1)
	}
	gMutex.Unlock()
	if traceIo {
		log.Debug("AVPipeWriteOutput", "fd", fd, "sz", sz)
	}

	if h.getOutTable(int64(fd)) == nil {
		msg := fmt.Sprintf("OutWriterX outTable entry is NULL, fd=%d", fd)
		panic(msg)
	}

	gobuf := C.GoBytes(unsafe.Pointer(buf), sz)
	n, err := h.OutWriter(fd, gobuf)
	if err != nil {
		return C.int(-1)
	}

	return C.int(n)
}

func (h *ioHandler) OutWriter(fd C.int64_t, buf []byte) (int, error) {
	outHandler := h.getOutTable(int64(fd))
	n, err := outHandler.Write(buf)
	if traceIo {
		log.Debug("OutWriter written", "n", n, "error", err)
	}
	return n, err
}

//export AVPipeSeekOutput
func AVPipeSeekOutput(handler C.int64_t, fd C.int64_t, offset C.int64_t, whence C.int) C.int {
	gMutex.Lock()
	h := gHandlers[int64(handler)]
	if h == nil {
		gMutex.Unlock()
		return C.int(-1)
	}
	gMutex.Unlock()
	n, err := h.OutSeeker(fd, offset, whence)
	if err != nil {
		return C.int(-1)
	}
	return C.int(n)
}

func (h *ioHandler) OutSeeker(fd C.int64_t, offset C.int64_t, whence C.int) (int64, error) {
	outHandler := h.getOutTable(int64(fd))
	n, err := outHandler.Seek(int64(offset), int(whence))
	log.Debug("OutSeeker", "err", err)
	return n, err
}

//export AVPipeCloseOutput
func AVPipeCloseOutput(handler C.int64_t, fd C.int64_t) C.int {
	gMutex.Lock()
	h := gHandlers[int64(handler)]
	if h == nil {
		gMutex.Unlock()
		return C.int(-1)
	}
	gMutex.Unlock()
	err := h.OutCloser(fd)
	if err != nil {
		return C.int(-1)
	}

	return C.int(0)
}

func (h *ioHandler) OutCloser(fd C.int64_t) error {
	outHandler := h.getOutTable(int64(fd))
	err := outHandler.Close()
	log.Debug("OutCloser()", "fd", int64(fd), "error", err)
	return err
}

//export AVPipeStatOutput
func AVPipeStatOutput(handler C.int64_t, fd C.int64_t, avp_stat C.avp_stat_t, stat_args unsafe.Pointer) C.int {
	gMutex.Lock()
	h := gHandlers[int64(handler)]
	if h == nil {
		gMutex.Unlock()
		return C.int(-1)
	}
	gMutex.Unlock()

	err := h.OutStat(fd, avp_stat, stat_args)
	if err != nil {
		return C.int(-1)
	}

	return C.int(0)
}

func (h *ioHandler) OutStat(fd C.int64_t, avp_stat C.avp_stat_t, stat_args unsafe.Pointer) error {
	var err error
	outHandler := h.getOutTable(int64(fd))
	if outHandler == nil {
		return fmt.Errorf("OutStat nil handler, fd=%d", int64(fd))
	}

	switch avp_stat {
	case C.out_stat_bytes_written:
		statArgs := *(*uint64)(stat_args)
		err = outHandler.Stat(AV_OUT_STAT_BYTES_WRITTEN, &statArgs)
	case C.out_stat_decoding_start_pts:
		statArgs := *(*uint64)(stat_args)
		err = outHandler.Stat(AV_OUT_STAT_DECODING_START_PTS, &statArgs)
	case C.out_stat_encoding_end_pts:
		statArgs := *(*uint64)(stat_args)
		err = outHandler.Stat(AV_OUT_STAT_ENCODING_END_PTS, &statArgs)
	}

	return err
}

//export CLog
func CLog(msg *C.char) C.int {
	m := C.GoString((*C.char)(unsafe.Pointer(msg)))
	log.Info(m)
	return C.int(0)
}

//export CDebug
func CDebug(msg *C.char) C.int {
	m := C.GoString((*C.char)(unsafe.Pointer(msg)))
	log.Debug(m)
	return C.int(len(m))
}

//export CInfo
func CInfo(msg *C.char) C.int {
	m := C.GoString((*C.char)(unsafe.Pointer(msg)))
	log.Info(m)
	return C.int(len(m))
}

//export CWarn
func CWarn(msg *C.char) C.int {
	m := C.GoString((*C.char)(unsafe.Pointer(msg)))
	log.Warn(m)
	return C.int(len(m))
}

//export CError
func CError(msg *C.char) C.int {
	m := C.GoString((*C.char)(unsafe.Pointer(msg)))
	log.Error(m)
	return C.int(len(m))
}

func SetCLoggers() {
	C.set_loggers()
}

// GetVersion ...
func Version() string {
	return C.GoString((*C.char)(unsafe.Pointer(C.avpipe_version())))
}

// params: transcoding parameters
// url: input filename that has to be transcoded
func Tx(params *TxParams, url string, debugFrameLevel bool) int {

	// Convert TxParams to C.txparams_t
	if params == nil {
		log.Error("Failed transcoding, params is not set.")
		return -1
	}

	// same field order as avpipe_xc.h
	cparams := &C.txparams_t{
		// bypass_transcoding handled below
		format:                 C.CString(params.Format),
		start_time_ts:          C.int64_t(params.StartTimeTs),
		skip_over_pts:          C.int64_t(params.SkipOverPts),
		start_pts:              C.int64_t(params.StartPts),
		duration_ts:            C.int64_t(params.DurationTs),
		start_segment_str:      C.CString(params.StartSegmentStr),
		video_bitrate:          C.int(params.VideoBitrate),
		audio_bitrate:          C.int(params.AudioBitrate),
		sample_rate:            C.int(params.SampleRate),
		crf_str:                C.CString(params.CrfStr),
		rc_max_rate:            C.int(params.RcMaxRate),
		rc_buffer_size:         C.int(params.RcBufferSize),
		seg_duration_ts:        C.int64_t(params.SegDurationTs),
		seg_duration:           C.CString(params.SegDuration),
		start_fragment_index:   C.int(params.StartFragmentIndex),
		force_keyint:           C.int(params.ForceKeyInt),
		ecodec:                 C.CString(params.Ecodec),
		dcodec:                 C.CString(params.Dcodec),
		enc_height:             C.int(params.EncHeight),
		enc_width:              C.int(params.EncWidth),
		crypt_iv:               C.CString(params.CryptIV),
		crypt_key:              C.CString(params.CryptKey),
		crypt_kid:              C.CString(params.CryptKID),
		crypt_key_url:          C.CString(params.CryptKeyURL),
		crypt_scheme:           C.crypt_scheme_t(params.CryptScheme),
		tx_type:                C.tx_type_t(params.TxType),
		watermark_text:         C.CString(params.WatermarkText),
		watermark_xloc:         C.CString(params.WatermarkXLoc),
		watermark_yloc:         C.CString(params.WatermarkYLoc),
		watermark_relative_sz:  C.float(params.WatermarkRelativeSize),
		watermark_font_color:   C.CString(params.WatermarkFontColor),
		watermark_shadow:       C.int(0),
		watermark_shadow_color: C.CString(params.WatermarkShadowColor),
		audio_index:            C.int(params.AudioIndex),
		bypass_transcoding:     C.int(0),
		seekable:               C.int(0),
		// seekable, bypass, and shadow handled below
	}

	if params.BypassTranscoding {
		cparams.bypass_transcoding = C.int(1)
	}

	if params.Seekable {
		cparams.seekable = C.int(1)
	}

	if params.WatermarkShadow {
		cparams.watermark_shadow = C.int(1)
	}

	var debugFrameLevelInt int
	if debugFrameLevel {
		debugFrameLevelInt = 1
	} else {
		debugFrameLevelInt = 0
	}

	rc := C.tx((*C.txparams_t)(unsafe.Pointer(cparams)), C.CString(url), C.int(debugFrameLevelInt))

	delete(gURLInputOpeners, url)
	delete(gURLOutputOpeners, url)

	return int(rc)
}

func ChannelLayoutName(nbChannels, channelLayout int) string {
	channelName := C.avpipe_channel_name(C.int(nbChannels), C.int(channelLayout))
	if unsafe.Pointer(channelName) != C.NULL {
		channelLayoutName := C.GoString((*C.char)(unsafe.Pointer(channelName)))
		return channelLayoutName
	}

	return ""
}

func GetPixelFormatName(pixFmt int) string {
	pName := C.get_pix_fmt_name(C.int(pixFmt))
	if unsafe.Pointer(pName) != C.NULL {
		pixelFormatName := C.GoString((*C.char)(unsafe.Pointer(pName)))
		return pixelFormatName
	}

	return ""
}

func GetProfileName(codecId int, profile int) string {
	pName := C.get_profile_name(C.int(codecId), C.int(profile))
	if unsafe.Pointer(pName) != C.NULL {
		profileName := C.GoString((*C.char)(unsafe.Pointer(pName)))
		return profileName
	}

	return ""
}

func Probe(url string, seekable bool) (*ProbeInfo, error) {
	var cprobe *C.txprobe_t
	var cseekable C.int

	if seekable {
		cseekable = C.int(1)
	} else {
		cseekable = C.int(0)
	}

	rc := C.probe(C.CString(url), cseekable, (**C.txprobe_t)(unsafe.Pointer(&cprobe)))
	if int(rc) <= 0 {
		return nil, fmt.Errorf("Probing failed")
	}

	probeInfo := &ProbeInfo{}
	probeInfo.StreamInfo = make([]StreamInfo, int(rc))
	probeArray := (*[1 << 10]C.stream_info_t)(unsafe.Pointer(cprobe.stream_info))
	for i := 0; i < int(rc); i++ {
		probeInfo.StreamInfo[i].StreamIndex = int(probeArray[i].stream_index)
		probeInfo.StreamInfo[i].CodecType = AVMediaTypeNames[AVMediaType(probeArray[i].codec_type)]
		probeInfo.StreamInfo[i].CodecID = int(probeArray[i].codec_id)
		probeInfo.StreamInfo[i].CodecName = C.GoString((*C.char)(unsafe.Pointer(&probeArray[i].codec_name)))
		probeInfo.StreamInfo[i].DurationTs = int64(probeArray[i].duration_ts)
		probeInfo.StreamInfo[i].TimeBase = big.NewRat(int64(probeArray[i].time_base.num), int64(probeArray[i].time_base.den))
		probeInfo.StreamInfo[i].NBFrames = int64(probeArray[i].nb_frames)
		probeInfo.StreamInfo[i].StartTime = int64(probeArray[i].start_time)
		if int64(probeArray[i].avg_frame_rate.den) != 0 {
			probeInfo.StreamInfo[i].AvgFrameRate = big.NewRat(int64(probeArray[i].avg_frame_rate.num), int64(probeArray[i].avg_frame_rate.den))
		} else {
			probeInfo.StreamInfo[i].AvgFrameRate = big.NewRat(int64(probeArray[i].avg_frame_rate.num), int64(1))
		}
		if int64(probeArray[i].frame_rate.den) != 0 {
			probeInfo.StreamInfo[i].FrameRate = big.NewRat(int64(probeArray[i].frame_rate.num), int64(probeArray[i].frame_rate.den))
		} else {
			probeInfo.StreamInfo[i].FrameRate = big.NewRat(int64(probeArray[i].frame_rate.num), int64(1))
		}
		probeInfo.StreamInfo[i].SampleRate = int(probeArray[i].sample_rate)
		probeInfo.StreamInfo[i].Channels = int(probeArray[i].channels)
		probeInfo.StreamInfo[i].ChannelLayout = int(probeArray[i].channel_layout)
		probeInfo.StreamInfo[i].TicksPerFrame = int(probeArray[i].ticks_per_frame)
		probeInfo.StreamInfo[i].BitRate = int64(probeArray[i].bit_rate)
		if probeArray[i].has_b_frames > 0 {
			probeInfo.StreamInfo[i].Has_B_Frames = true
		} else {
			probeInfo.StreamInfo[i].Has_B_Frames = false
		}
		probeInfo.StreamInfo[i].Width = int(probeArray[i].width)
		probeInfo.StreamInfo[i].Height = int(probeArray[i].height)
		probeInfo.StreamInfo[i].PixFmt = int(probeArray[i].pix_fmt)
		if int64(probeArray[i].sample_aspect_ratio.den) != 0 {
			probeInfo.StreamInfo[i].SampleAspectRatio = big.NewRat(int64(probeArray[i].sample_aspect_ratio.num), int64(probeArray[i].sample_aspect_ratio.den))
		} else {
			probeInfo.StreamInfo[i].SampleAspectRatio = big.NewRat(int64(probeArray[i].sample_aspect_ratio.num), int64(1))
		}
		if int64(probeArray[i].display_aspect_ratio.den) != 0 {
			probeInfo.StreamInfo[i].DisplayAspectRatio = big.NewRat(int64(probeArray[i].display_aspect_ratio.num), int64(probeArray[i].display_aspect_ratio.den))
		} else {
			probeInfo.StreamInfo[i].DisplayAspectRatio = big.NewRat(int64(probeArray[i].display_aspect_ratio.num), int64(1))
		}
		probeInfo.StreamInfo[i].FieldOrder = AVFieldOrderNames[AVFieldOrder(probeArray[i].field_order)]
		probeInfo.StreamInfo[i].Profile = int(probeArray[i].profile)
		probeInfo.StreamInfo[i].Level = int(probeArray[i].level)
	}

	probeInfo.ContainerInfo.FormatName = C.GoString((*C.char)(unsafe.Pointer(cprobe.container_info.format_name)))
	probeInfo.ContainerInfo.Duration = float64(cprobe.container_info.duration)

	C.free(unsafe.Pointer(cprobe.stream_info))
	C.free(unsafe.Pointer(cprobe))

	delete(gURLInputOpeners, url)
	delete(gURLOutputOpeners, url)

	return probeInfo, nil
}

// Returns a handle and error (if there is any error)
// In case of error the handle would be zero
func TxInit(params *TxParams, url string, debugFrameLevel bool) (int32, error) {
	// Convert TxParams to C.txparams_t
	if params == nil {
		log.Error("Failed transcoding, params is not set.")
		return -1, fmt.Errorf("TxParams is nil")
	}

	// same field order as avpipe_xc.h
	cparams := &C.txparams_t{
		// bypass_transcoding handled below
		format:                 C.CString(params.Format),
		start_time_ts:          C.int64_t(params.StartTimeTs),
		skip_over_pts:          C.int64_t(params.SkipOverPts),
		start_pts:              C.int64_t(params.StartPts),
		duration_ts:            C.int64_t(params.DurationTs),
		start_segment_str:      C.CString(params.StartSegmentStr),
		video_bitrate:          C.int(params.VideoBitrate),
		audio_bitrate:          C.int(params.AudioBitrate),
		sample_rate:            C.int(params.SampleRate),
		crf_str:                C.CString(params.CrfStr),
		rc_max_rate:            C.int(params.RcMaxRate),
		rc_buffer_size:         C.int(params.RcBufferSize),
		seg_duration_ts:        C.int64_t(params.SegDurationTs),
		seg_duration:           C.CString(params.SegDuration),
		start_fragment_index:   C.int(params.StartFragmentIndex),
		force_keyint:           C.int(params.ForceKeyInt),
		ecodec:                 C.CString(params.Ecodec),
		dcodec:                 C.CString(params.Dcodec),
		enc_height:             C.int(params.EncHeight),
		enc_width:              C.int(params.EncWidth),
		crypt_iv:               C.CString(params.CryptIV),
		crypt_key:              C.CString(params.CryptKey),
		crypt_kid:              C.CString(params.CryptKID),
		crypt_key_url:          C.CString(params.CryptKeyURL),
		crypt_scheme:           C.crypt_scheme_t(params.CryptScheme),
		tx_type:                C.tx_type_t(params.TxType),
		watermark_text:         C.CString(params.WatermarkText),
		watermark_xloc:         C.CString(params.WatermarkXLoc),
		watermark_yloc:         C.CString(params.WatermarkYLoc),
		watermark_relative_sz:  C.float(params.WatermarkRelativeSize),
		watermark_font_color:   C.CString(params.WatermarkFontColor),
		watermark_shadow:       C.int(0),
		watermark_shadow_color: C.CString(params.WatermarkShadowColor),
		bypass_transcoding:     C.int(0),
		seekable:               C.int(0),

		audio_index: C.int(params.AudioIndex),
		// seekable, bypass, and shadow handled below
	}

	if params.BypassTranscoding {
		cparams.bypass_transcoding = C.int(1)
	}

	if params.Seekable {
		cparams.seekable = C.int(1)
	}

	if params.WatermarkShadow {
		cparams.watermark_shadow = C.int(1)
	}

	var debugFrameLevelInt int
	if debugFrameLevel {
		debugFrameLevelInt = 1
	} else {
		debugFrameLevelInt = 0
	}

	handle := C.tx_init((*C.txparams_t)(unsafe.Pointer(cparams)), C.CString(url), C.int(debugFrameLevelInt))
	if handle < C.int32_t(0) {
		return -1, fmt.Errorf("Tx initialization failed")
	}

	return int32(handle), nil
}

func TxRun(handle int32) error {
	rc := C.tx_run(C.int32_t(handle))
	if rc == 0 {
		return nil
	}

	return fmt.Errorf("TxRun failed with the handle=%d", handle)
}

func TxCancel(handle int32) error {
	rc := C.tx_cancel(C.int32_t(handle))
	if rc == 0 {
		return nil
	}

	return fmt.Errorf("TxCancel failed with the handle=%d", handle)
}

// StreamInfoAsArray builds an array where each stream is at its corresponsing index
// by filling in non-existing index positions with codec type "unknown"
func StreamInfoAsArray(s []StreamInfo) []StreamInfo {
	maxIdx := 0
	for _, v := range s {
		if v.StreamIndex > maxIdx {
			maxIdx = v.StreamIndex
		}
	}
	a := make([]StreamInfo, maxIdx+1)
	for i, _ := range a {
		a[i].StreamIndex = i
		a[i].CodecType = AVMediaTypeNames[AVMediaType(AVMEDIA_TYPE_UNKNOWN)]
	}
	for _, v := range s {
		a[v.StreamIndex] = v
	}
	return a
}
