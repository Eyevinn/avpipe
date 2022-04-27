# Text watermarking
./bin/exc -seg-duration 30 -xc-type video -f media/creed_1_min.mov -wm-text "TEST WATERMARK" -wm-color black -wm-relative-size 0.05 -wm-xloc W/2 -wm-yloc H*0.9 -format fmp4-segment
./bin/exc -seg-duration 30 -xc-type video -f media/creed_1_min.mov -wm-text "%{pts\\:gmtime\\:1602968400\\:%d-%m-%Y %T}" -wm-color black -wm-relative-size 0.05 -wm-xloc W/2 -wm-yloc H*0.9 -format fmp4-segment

# Timecode watermarking
./bin/elvxc transcode --seg-duration 30 --xc-type video -f media/creed_1_min.mov --wm-timecode-rate 24 --wm-color black --wm-relative-size 0.05 --wm-xloc W/2 --wm-yloc H*0.9 --format fmp4-segment --wm-timecode "00\:00\:00\:00"

# Image watermarking
./bin/elvxc transcode --wm-overlay ./fox_watermark.png --wm-overlay-type png --wm-xloc "main_w/2-overlay_w/2" --wm-yloc main_h*0.7 -f O/O1-mez/fsegment0-00001.mp4 --seg-duration-ts 48000 --xc-type video

./bin/exc -wm-overlay ./fox_watermark.png -wm-xloc "main_w/2-overlay_w/2" -wm-yloc main_h*0.7 -f O/O1-mez/fsegment0-00001.mp4 -seg-duration-ts 48000 -xc-type video

# Make HDR mezzanines
./bin/exc -seg-duration 30.03 -f across_the_universe_2160p_h265_veryslow_segmented_60_sec.mp4 -e libx265 -xc-type video -format fmp4-segment -max-cll "1514,172" -master-display "G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(40000000,50)"

./bin/exc -seg-duration 30.03 -f across_the_universe_2160p_h265_veryslow_segmented_60_sec.mp4 -e libx265 -xc-type video -format fmp4-segment -bitdepth 10

# Muxing audio/video
./bin/exc -command mux -mux-spec scripts/sample_mux_spec_abr.txt -f muxed.mp4

# Listen in RTMP mode and make mezzanines
./bin/exc -f rtmp://localhost:5000/test001 -xc-type all -format fmp4-segment -video-seg-duration-ts 480480 -audio-seg-duration-ts 1441440 -force-keyint 4 -listen 1


# Audio join
./bin/elvxc transcode -f ./media/AGAIG-clip-2mono.mp4 --xc-type audio-join --audio-index 1,2 --format fmp4-segment --seg-duration 30

ffmpeg-4.2.2-amd64-static/ffmpeg -y -i ./media/AGAIG-clip-2mono.mp4 -vn -filter_complex "[0:1][0:2]join=inputs=2:channel_layout=stereo[aout]" -map [aout] -acodec aac -b:a 128k 'out-audio.mp4'


# Audio pan
./bin/exc -f ../media/MGM/multichannel_audio_clip.mov -xc-type audio-pan -filter-descriptor "[0:1]pan=stereo|c0<c1+0.707*c2|c1<c2+0.707*c1[aout]" -format fmp4-segment -seg-duration 30

ffmpeg -i ../media/MGM/multichannel_audio_clip.mov -vn  -filter_complex "[0:1]pan=stereo|c0<c1+0.707*c2|c1<c2+0.707*c1[aout]" -map [aout]  -acodec aac -b:a 128k bb.mp4


# Audio merge (not functional yet)
./bin/exc -f ../media/BOB923HL_clip_timebase_1001_60000.MXF -xc-type audio-merge -filter-descriptor "[0:1][0:2][0:3][0:5][0:6]amerge=inputs=5,pan=stereo|c0<c0+c3+0.707*c2|c1<c1+c4+0.707*c2[out]" -format fmp4-segment -seg-duration 30 -audio-index 1,2,3,5,6 -channel-layout stereo

ffmpeg-4.2.2-amd64-static/ffmpeg -y -i ../media/MGM/BOB923HL_clip_timebase_1001_60000.MXF -vn -filter_complex "[0:1][0:2][0:3][0:5][0:6]amerge=inputs=5,pan=stereo|c0<c0+c3+0.707*c2|c1<c1+c4+0.707*c2[aout]" -map [aout] -acodec aac -b:a 128k 'BOB1003HL.MXF.audio.mp4'

$FFMPEG_BIN/ffmpeg -listen 1 -i rtmp://192.168.90.202:1935/rtmp/XQjNir3S -c copy -flags +global_header -f segment -segment_time 60 -segment_format_options movflags=+faststart -reset_timestamps 1 test%d.mp4


# Extract frames every 2s (default 10s if interval is not set)
./bin/exc -f ../media/ErsteChristmas.mp4 -format image2 -e mjpeg -xc-type extract-images -extract-image-interval-ts 25600
# Extract specified frames
./bin/exc -f ../media/ErsteChristmas.mp4 -format image2 -e mjpeg -xc-type extract-images -extract-images-ts "64000,128000,1152000"
