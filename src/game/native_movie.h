#pragma once
// Native movie decoding (option decodificar_videos, off by default): the game's libmpeg picture
// decoding (sceMpegGetPicture through the IPU) replaced by src/ipu/mpeg2_decoder, reading the
// game's own video ring, for processors too slow to emulate it at full speed.
#include <cstdint>
#include <string>

class Cpu;

void installNativeMovie(Cpu& cpu);

// Development check (codport --mpeg2-test file.pss [bmp folder] [every]): decodes a PSS movie's
// video natively, printing a hash of each picture in libmpeg's layout, in display order.
int mpeg2TestFile(const std::string& pss, const std::string& bmpDir, int every);
