package live

import (
	"io"
	"net"
	"strings"
	"time"
)

type TsReader struct {
	addr     string // For example ":21001" (for localhost port 21001)
	pktLimit int
	w        io.Writer
	done     chan bool

	NextSkipOverPts int64 // Bogus field for compat with HLS
}

// Deprecated
func NewTsReader(addr string, w io.Writer) *TsReader {

	tsr := &TsReader{
		addr: addr,
		w:    w,
	}

	var err error
	if strings.HasPrefix(addr, "/") || strings.HasPrefix(addr, "./") {
		err = tsr.serveFromFile(w)
	} else {
		err = tsr.serveOneConnection(w)
	}
	if err != nil {
		log.Error("TsReader failed", "err", err)
	}

	return tsr
}

// NewTsReaderV2  creates a UDP MPEG-TS reader and returns an io.Reader
// Starts the necessary goroutines - when the returned reader is closed, it stops
// all goroutines and cleans up.
func NewTsReaderV2(addr string) io.ReadWriter {

	rwb := NewRWBuffer(10000)

	tsr := &TsReader{
		addr: addr,
		w:    rwb,
	}

	var err error
	if strings.HasPrefix(addr, "/") || strings.HasPrefix(addr, "./") {
		err = tsr.serveFromFile(rwb)
	} else {
		err = tsr.serveOneConnection(rwb)
	}
	if err != nil {
		log.Error("TsReader failed", "err", err)
	}

	return rwb
}

func readUdp(pc net.PacketConn, w io.Writer) error {

	// Assume that Close() is implemented, and that writer is not used after
	// this call
	defer w.(*RWBuffer).Close(RWBufferWriteClosed)

	// Stop recording if nothing was read for timeout
	timeout := 5 * time.Second

	bytesRead := 0
	buf := make([]byte, 65536)

	for {
		if err := pc.SetReadDeadline(time.Now().Add(timeout)); err != nil {
			return err
		}

		n, sender, err := pc.ReadFrom(buf)
		bytesRead += n
		if err != nil {
			if err.(net.Error).Timeout() {
				if bytesRead == 0 {
					continue // waiting for stream start
				}
				log.Info("Stopped receiving UDP packets",
					"timeout", timeout, "bytesRead", bytesRead)
				break
			}
			log.Error("UDP read failed", "err", err, "sender", sender)
			return err
		}
		// log.Debug("te_recorder: packet received", "bytes", n, "from", sender.String(), "b0", buf[0], "b1", buf[1], "b2", buf[2])

		t := time.Now()
		bw, err := w.Write(buf[:n])
		if err != nil || bw != n {
			log.Error("Failed to write UDP packet", "err", err, "bw", bw, "n", n, "sender", sender)
			return err
		}
		if time.Since(t) > time.Millisecond*10 || bw > 1500 {
			log.Warn("Writing UDP to avpipe took longer than expected", "timeSpent", time.Since(t), "written", bw)
		}
	}
	return nil
}

func (tsr *TsReader) serveOneConnection(w io.Writer) (err error) {

	sAddr, err := net.ResolveUDPAddr("udp", tsr.addr)
	if err != nil {
		return
	}
	conn, err := net.ListenUDP("udp", sAddr)
	if err != nil {
		return
	}
	// TODO: Make if a config param (RM)
	conn.SetReadBuffer(16 * 1024 * 1024)

	log.Info("ts_recorder: server: accepted")

	go func() {
		if err := readUdp(conn, w); err != nil {
			log.Error("Failed reading UDP stream", "err", err)
			// TODO: Error does not bubble up
		}
	}()

	return
}

func (tsr *TsReader) serveFromFile(w io.Writer) (err error) {

	/* Not implemented */
	return
}
