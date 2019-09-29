// Copyright 2019 Saferwall. All rights reserved.
// Use of this source code is governed by Apache v2 license
// license that can be found in the LICENSE file.

package main

import (
	"log"
	"testing"
)

func TestScanFile(t *testing.T) {
	client := Init()
	res, _ := ScanFile(client, "/samples/eicar")
	log.Println(res)

}