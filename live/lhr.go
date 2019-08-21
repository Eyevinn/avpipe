// LiveHlsReader provides a reader interface to a live HLS stream - it reads a specified number of segments
// on outputs one aggregate MPEG-TS buffer
package live

import (
	"errors"
	"github.com/grafov/m3u8"
	"io"
	"log"
	"net/http"
	"net/url"
	"os"
	"path"
	"time"
)

type LiveHlsReader struct {
	sequence          int
	client            http.Client
	url               *url.URL
	numSegmentsNeeded int
	numSegmentsRead   int
}

func NewLiveHlsReader(url *url.URL) *LiveHlsReader {

	lhr := LiveHlsReader{
		sequence: -1,
		client:   http.Client{},
		url:      url,
	}
	return &lhr
}

func (lhr *LiveHlsReader) openUrl(u *url.URL) (io.ReadCloser, error) {

	req, err := http.NewRequest("GET", u.String(), nil)
	if err != nil {
		return nil, err
	}

	resp, err := lhr.client.Do(req)
	if err != nil {
		return nil, err
	}

	if resp.StatusCode != 200 {
		log.Println("Failed HTTP", "status", resp.StatusCode, "URL", u.String())
		return nil, nil
	}

	return resp.Body, nil
}

// resolve returns an absolute URL
func resolve(urlStr string, base *url.URL) (*url.URL, error) {

	url, err := url.Parse(urlStr)
	if err != nil {
		return nil, err
	}

	if url.IsAbs() {
		return url, nil
	}

	return base.ResolveReference(url), nil
}

func (lhr *LiveHlsReader) saveToFile(u *url.URL) error {
	fileName := path.Base(u.Path)

	out, err := os.Create(fileName)
	if err != nil {
		return err
	}
	defer out.Close()

	content, err := lhr.openUrl(u)
	if err != nil {
		return err
	}
	defer content.Close()

	_, err = io.Copy(out, content)
	if err != nil {
		return err
	}

	log.Println("Saved file", fileName)

	return nil
}

func (lhr *LiveHlsReader) readSegment(u *url.URL, w io.Writer) error {
	content, err := lhr.openUrl(u)
	if err != nil {
		return err
	}
	defer content.Close()

	_, err = io.Copy(w, content)
	if err != nil {
		return err
	}

	return nil

}

func (lhr *LiveHlsReader) readMasterPlaylist(u *url.URL) ([]*m3u8.Variant, error) {

	content, err := lhr.openUrl(u)
	if err != nil {
		return nil, err
	}
	defer content.Close()

	playlist, listType, err := m3u8.DecodeFrom(content, true)
	if err != nil {
		return nil, err
	}

	if listType != m3u8.MASTER {
		return nil, errors.New("Invalid playlist")
	}

	masterPlaylist := playlist.(*m3u8.MasterPlaylist)
	return masterPlaylist.Variants, nil
}

func (lhr *LiveHlsReader) readPlaylist(u *url.URL, startSequence, numSegments int, w io.Writer) (int, int, error) {

	if numSegments <= 0 {
		return -1, -1, errors.New("Invalid parameter")
	}

	log.Println("Reading playlist", "startSequence", startSequence, "numSegments", numSegments)
	content, err := lhr.openUrl(u)
	if err != nil {
		return -1, -1, err
	}
	defer content.Close()

	playlist, listType, err := m3u8.DecodeFrom(content, true)
	if err != nil {
		return -1, -1, err
	}

	if listType != m3u8.MEDIA {
		return -1, -1, err
	}

	mediaPlaylist := playlist.(*m3u8.MediaPlaylist)

	// We consider sequence numbers 0-based so the first segment in the manifest will have
	// the sequence number equal to the manifest's media sequence
	seqNo := int(mediaPlaylist.SeqNo)
	numRead := 0
	lastSeqNo := -1

	for _, segment := range mediaPlaylist.Segments {
		if segment == nil {
			continue // Strangely the API returns an nil segment at the end
		}
		if seqNo < startSequence {
			seqNo++
			continue
		}
		if numRead == numSegments {
			break
		}
		msURL, err := resolve(segment.URI, u)
		if err != nil {
			log.Fatal("Failed to retrieve segment URL " + err.Error())
		}

		// lhr.saveToFile(msURL)
		lhr.readSegment(msURL, w)
		numRead++
		lastSeqNo = seqNo
		seqNo++
	}
	return numRead, lastSeqNo, nil
}

// fill() reads HLS input as indicated by parameters startSesquence and numSegments
// and writes it out to the provided io.Writer
// If startSequence is -1, it starts with the first sequence it gets
// The sequence number is 0-based (i.e. the first segment has sequence number 0)
func (lhr *LiveHlsReader) Fill(startSequence, numSegments int, w io.Writer) error {

	variants, err := lhr.readMasterPlaylist(lhr.url)
	if err != nil {
		return err
	}

	for n := 0; n < numSegments; {

		for _, variant := range variants {
			if variant == nil {
				log.Println("WARNING: invalid variant (nil)")
				continue
			}

			// PENDING(SSS) - check if we have multiple variants and if so only
			// read the one we are supposed to
			msURL, err := resolve(variant.URI, lhr.url)
			if err != nil {
				log.Fatal("Failed to resolve URL " + err.Error())
			}
			numRead, lastSeq, err := lhr.readPlaylist(msURL, startSequence, numSegments-n, w)
			if err != nil {
				log.Fatal("Failed to read playlist " + err.Error())
			}
			if numRead > 0 {
				n += numRead
				startSequence = lastSeq + 1
			} else {
				// Wait for a new segment
				time.Sleep(time.Duration(1000 * time.Millisecond))
			}
		}
	}
	return nil
}
