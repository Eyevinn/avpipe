include ./rules.make

TOP_DIR ?= $(shell pwd)
SUBDIRS=utils libavpipe etx avcmd

SRCS=avpipe_handler.c
OBJS=$(SRCS:%.c=$(BINDIR)/%.o)

.PHONY: all test clean

.DEFAULT_GOAL := dynamic

all install: copy_libs check-env
	@for dir in $(SUBDIRS); do \
	echo "Making $@ in $$dir..."; \
	(cd $$dir; make $@) || exit 1; \
	done

dynamic: copy_libs_all all

clean: lclean
	@for dir in $(SUBDIRS); do \
	echo "Making $@ in $$dir..."; \
	(cd $$dir; make $@) || exit 1; \
	done
	@rm -rf test_out
	@rm -rf *.log

copy_libs:
	@(if [ ! -d $(LIBDIR) ]; then mkdir $(LIBDIR); fi)
	@(if [ ! -d $(INCDIR) ]; then mkdir $(INCDIR); fi)
	cp ${ELV_TOOLCHAIN_DIST_PLATFORM}/lib/libav*.a ${LIBDIR}
	cp ${ELV_TOOLCHAIN_DIST_PLATFORM}/lib/libswresample.a ${LIBDIR}
	cp ${ELV_TOOLCHAIN_DIST_PLATFORM}/lib/libswscale.a ${LIBDIR}
	cp ${ELV_TOOLCHAIN_DIST_PLATFORM}/lib/libpostproc.a ${LIBDIR}
	cp -r ${ELV_TOOLCHAIN_DIST_PLATFORM}/include/* ${INCDIR}

copy_libs_all:
	@(if [ ! -d $(LIBDIR) ]; then mkdir $(LIBDIR); fi)
	@(if [ ! -d $(INCDIR) ]; then mkdir $(INCDIR); fi)
	cp ${ELV_TOOLCHAIN_DIST_PLATFORM}/lib/libav* ${LIBDIR}
	cp ${ELV_TOOLCHAIN_DIST_PLATFORM}/lib/libswresample* ${LIBDIR}
	cp ${ELV_TOOLCHAIN_DIST_PLATFORM}/lib/libswscale* ${LIBDIR}
	cp ${ELV_TOOLCHAIN_DIST_PLATFORM}/lib/libpostproc* ${LIBDIR}
	cp -r ${ELV_TOOLCHAIN_DIST_PLATFORM}/include/* ${INCDIR}

avpipe:
	CGO_CFLAGS="-I./include" CGO_LDFLAGS="-L${TOP_DIR}/lib -lavcodec -lavformat -lavfilter -lavpipe -lavdevice -lswresample -libavresample -lswscale -lavutil -lpostproc -lutils -lz -lm -ldl -lvdpau -lva -lX11 -lpthread" go build -v
	mkdir -p ./O

libavpipego: $(OBJS)
	@(if [ ! -d $(LIBDIR) ]; then mkdir $(LIBDIR); fi)
	@echo Making libavpipe_handler
	@ld -r -o $(LIBDIR)/libavpipe_handler.a $?

$(BINDIR)/%.o: %.c
	@(if [ ! -d $(BINDIR) ]; then mkdir $(BINDIR); fi)
	@echo "Compiling " $<
	gcc ${FLAGS} ${INCDIRS} -c $< -o $@

lclean:
	@rm -rf lib bin include

clean-test:
	@rm -rf AAC2AAC AC3TsAC3Mez \
            AC3TsACCMez Downmix2ACCMez \
            MP2TsACCMez HEVC_H264 H265MXF \
            HEVC_H265ABR V2SingleABRTranscode* \
            Audio*Muxing Video*Muxing MuxingOutput \
            IrregularTsMezMaker* \
            2Mono1Stereo 2Channel1Stereo \
            SingleABRTranscode* avpipe-test*.log

check-env:
ifndef ELV_TOOLCHAIN_DIST_PLATFORM
  $(error ELV_TOOLCHAIN_DIST_PLATFORM is undefined)
endif

test:
	@cd ./media
	@(if ! [ -x "command -v gsutil" ]; then echo "gsutil could not be found, install gsutil"; exit 1; fi) || exit 1
	@gsutil -m cp 'gs://qluvio-test-assets/*' .
	@go test --timeout 10000s

