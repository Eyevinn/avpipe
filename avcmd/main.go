package main

import (
	"fmt"
	"os"

	"github.com/eluv-io/log-go"

	"github.com/qluvio/avpipe"
	"github.com/qluvio/avpipe/avcmd/cmd"
	"github.com/spf13/cobra"
)

func main() {
	cmdRoot := &cobra.Command{
		Use:          "avcmd",
		Short:        "Audio Video Command",
		Long:         "",
		SilenceUsage: false,
	}

	log.SetDefault(&log.Config{
		Level:   "debug",
		Handler: "text",
		File: &log.LumberjackConfig{
			Filename:  "avcmd.log",
			LocalTime: true,
		},
	})
	avpipe.SetCLoggers()

	log.Info("Starting avcmd", "version", avpipe.Version())

	err := cmd.InitTranscode(cmdRoot)
	if err != nil {
		fmt.Println(err)
		os.Exit(1)
	}

	err = cmd.InitMux(cmdRoot)
	if err != nil {
		fmt.Println(err)
		os.Exit(1)
	}

	err = cmd.WarmupStress(cmdRoot)
	if err != nil {
		fmt.Println(err)
		os.Exit(1)
	}

	err = cmd.AnalyseLog(cmdRoot)
	if err != nil {
		fmt.Println(err)
		os.Exit(1)
	}

	err = cmd.Probe(cmdRoot)
	if err != nil {
		fmt.Println(err)
		os.Exit(1)
	}

	err = cmdRoot.Execute()
	if err != nil {
		fmt.Printf("Command failed\n")
		os.Exit(1)
	}
}
