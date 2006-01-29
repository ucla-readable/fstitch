#include <inc/lib.h>
#include <inc/malloc.h>
#include <inc/string.h>

const unsigned char demo_font_map[256][8] = {
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x3c, 0x42, 0xa5, 0x81, 0xa5, 0x99, 0x42, 0x3c}, {0x3c, 0x7e, 0xdb, 0xff, 0xdb, 0xe7, 0x7e, 0x3c}, {0x36, 0x7f, 0x7f, 0x7f, 0x3e, 0x1c, 0x08, 0x00},
	{0x08, 0x1c, 0x3e, 0x7f, 0x3e, 0x1c, 0x08, 0x00}, {0x1c, 0x1c, 0x6b, 0x7f, 0x6b, 0x08, 0x1c, 0x00}, {0x08, 0x1c, 0x3e, 0x7f, 0x3e, 0x08, 0x1c, 0x00}, {0x00, 0x00, 0x18, 0x3c, 0x3c, 0x18, 0x00, 0x00},
	{0xff, 0xff, 0xe7, 0xc3, 0xc3, 0xe7, 0xff, 0xff}, {0x00, 0x3c, 0x66, 0x42, 0x42, 0x66, 0x3c, 0x00}, {0xff, 0xc3, 0x99, 0xbd, 0xbd, 0x99, 0xc3, 0xff}, {0x0f, 0x03, 0x05, 0x79, 0xd8, 0xd8, 0x70, 0x00},
	{0x3c, 0x66, 0x66, 0x3c, 0x18, 0x7e, 0x18, 0x00}, {0x08, 0x0c, 0x0e, 0x0a, 0x08, 0x18, 0x38, 0x30}, {0x3f, 0x21, 0x3f, 0x21, 0x23, 0x67, 0xe6, 0xc0}, {0x08, 0x6b, 0x1c, 0x77, 0x1c, 0x6b, 0x08, 0x00},
	{0x80, 0xe0, 0xf8, 0xfe, 0xf8, 0xe0, 0x80, 0x00}, {0x01, 0x07, 0x1f, 0x7f, 0x1f, 0x07, 0x01, 0x00}, {0x08, 0x1c, 0x3e, 0x08, 0x3e, 0x1c, 0x08, 0x00}, {0x66, 0x66, 0x66, 0x66, 0x66, 0x00, 0x66, 0x00},
	{0x7f, 0xdb, 0xdb, 0x7b, 0x1b, 0x1b, 0x1b, 0x00}, {0x3e, 0x63, 0x38, 0x26, 0x32, 0x0e, 0x63, 0x3e}, {0x00, 0x00, 0x00, 0x00, 0x7e, 0x7e, 0x7e, 0x00}, {0x18, 0x3c, 0x7e, 0x18, 0x7e, 0x3c, 0x18, 0x7e},
	{0x18, 0x3c, 0x7e, 0x18, 0x18, 0x18, 0x18, 0x00}, {0x18, 0x18, 0x18, 0x18, 0x7e, 0x3c, 0x18, 0x00}, {0x00, 0x04, 0x06, 0x7f, 0x06, 0x04, 0x00, 0x00}, {0x00, 0x10, 0x30, 0x7f, 0x30, 0x10, 0x00, 0x00},
	{0x00, 0x00, 0x60, 0x60, 0x60, 0x7f, 0x00, 0x00}, {0x00, 0x24, 0x66, 0xff, 0x66, 0x24, 0x00, 0x00}, {0x00, 0x00, 0x08, 0x1c, 0x3e, 0x7f, 0x00, 0x00}, {0x00, 0x00, 0x7f, 0x3e, 0x1c, 0x08, 0x00, 0x00},
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x18, 0x3c, 0x3c, 0x18, 0x18, 0x00, 0x18, 0x00}, {0x36, 0x36, 0x36, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x36, 0x36, 0x7f, 0x36, 0x7f, 0x36, 0x36, 0x00},
	{0x08, 0x3e, 0x68, 0x3e, 0x0b, 0x3e, 0x08, 0x00}, {0x61, 0x63, 0x06, 0x0c, 0x18, 0x33, 0x63, 0x00}, {0x1c, 0x36, 0x1c, 0x39, 0x6e, 0x66, 0x3b, 0x00}, {0x0c, 0x0c, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00},
	{0x0c, 0x18, 0x30, 0x30, 0x30, 0x18, 0x0c, 0x00}, {0x18, 0x0c, 0x06, 0x06, 0x06, 0x0c, 0x18, 0x00}, {0x00, 0x24, 0x18, 0x7e, 0x18, 0x24, 0x00, 0x00}, {0x00, 0x18, 0x18, 0x7e, 0x18, 0x18, 0x00, 0x00},
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x30}, {0x00, 0x00, 0x00, 0x7e, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00}, {0x01, 0x03, 0x06, 0x0c, 0x18, 0x30, 0x60, 0x00},
	{0x3e, 0x63, 0x6b, 0x6b, 0x6b, 0x63, 0x3e, 0x00}, {0x0c, 0x1c, 0x3c, 0x0c, 0x0c, 0x0c, 0x3f, 0x00}, {0x3c, 0x66, 0x06, 0x0c, 0x18, 0x30, 0x7e, 0x00}, {0x3c, 0x66, 0x06, 0x1c, 0x06, 0x66, 0x3c, 0x00},
	{0x06, 0x0e, 0x1e, 0x36, 0x7f, 0x06, 0x06, 0x00}, {0x7f, 0x60, 0x7e, 0x03, 0x03, 0x63, 0x3e, 0x00}, {0x1e, 0x30, 0x60, 0x7e, 0x63, 0x63, 0x3e, 0x00}, {0x7f, 0x63, 0x06, 0x0c, 0x18, 0x18, 0x18, 0x00},
	{0x3e, 0x63, 0x63, 0x3e, 0x63, 0x63, 0x3e, 0x00}, {0x3e, 0x63, 0x63, 0x3f, 0x03, 0x06, 0x3c, 0x00}, {0x00, 0x18, 0x18, 0x00, 0x00, 0x18, 0x18, 0x00}, {0x00, 0x18, 0x18, 0x00, 0x00, 0x18, 0x18, 0x30},
	{0x06, 0x0c, 0x18, 0x30, 0x18, 0x0c, 0x06, 0x00}, {0x00, 0x00, 0x7e, 0x00, 0x00, 0x7e, 0x00, 0x00}, {0x30, 0x18, 0x0c, 0x06, 0x0c, 0x18, 0x30, 0x00}, {0x3c, 0x66, 0x0c, 0x18, 0x18, 0x00, 0x18, 0x00},
	{0x3e, 0x63, 0x6f, 0x6f, 0x6f, 0x60, 0x3f, 0x00}, {0x1c, 0x36, 0x63, 0x63, 0x7f, 0x63, 0x63, 0x00}, {0x7e, 0x63, 0x63, 0x7e, 0x63, 0x63, 0x7e, 0x00}, {0x3e, 0x63, 0x60, 0x60, 0x60, 0x63, 0x3e, 0x00},
	{0x7c, 0x66, 0x63, 0x63, 0x63, 0x66, 0x7c, 0x00}, {0x7e, 0x60, 0x60, 0x7c, 0x60, 0x60, 0x7e, 0x00}, {0x7e, 0x60, 0x60, 0x7c, 0x60, 0x60, 0x60, 0x00}, {0x3e, 0x63, 0x60, 0x6f, 0x63, 0x63, 0x3f, 0x00},
	{0x63, 0x63, 0x63, 0x7f, 0x63, 0x63, 0x63, 0x00}, {0x7e, 0x18, 0x18, 0x18, 0x18, 0x18, 0x7e, 0x00}, {0x0f, 0x06, 0x06, 0x06, 0x06, 0x66, 0x3c, 0x00}, {0x63, 0x66, 0x6c, 0x78, 0x6c, 0x66, 0x63, 0x00},
	{0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x7f, 0x00}, {0x63, 0x77, 0x7f, 0x6b, 0x6b, 0x63, 0x63, 0x00}, {0x63, 0x73, 0x7b, 0x6f, 0x67, 0x63, 0x63, 0x00}, {0x3e, 0x63, 0x63, 0x63, 0x63, 0x63, 0x3e, 0x00},
	{0x7e, 0x63, 0x63, 0x7e, 0x60, 0x60, 0x60, 0x00}, {0x3e, 0x63, 0x63, 0x63, 0x6d, 0x66, 0x3b, 0x00}, {0x7e, 0x63, 0x63, 0x7e, 0x6c, 0x66, 0x63, 0x00}, {0x3e, 0x63, 0x60, 0x3e, 0x03, 0x63, 0x3e, 0x00},
	{0xff, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00}, {0x63, 0x63, 0x63, 0x63, 0x63, 0x63, 0x3e, 0x00}, {0x63, 0x63, 0x63, 0x36, 0x36, 0x1c, 0x1c, 0x00}, {0x63, 0x63, 0x6b, 0x6b, 0x7f, 0x77, 0x63, 0x00},
	{0x63, 0x63, 0x36, 0x1c, 0x36, 0x63, 0x63, 0x00}, {0xc3, 0xc3, 0x66, 0x3c, 0x18, 0x18, 0x18, 0x00}, {0x7f, 0x03, 0x06, 0x1c, 0x30, 0x60, 0x7f, 0x00}, {0x3c, 0x30, 0x30, 0x30, 0x30, 0x30, 0x3c, 0x00},
	{0xc0, 0x60, 0x30, 0x18, 0x0c, 0x06, 0x03, 0x00}, {0x3c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x3c, 0x00}, {0x18, 0x3c, 0x66, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff},
	{0x18, 0x18, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x3e, 0x03, 0x3f, 0x63, 0x3f, 0x00}, {0x60, 0x60, 0x6e, 0x73, 0x63, 0x73, 0x6e, 0x00}, {0x00, 0x00, 0x3e, 0x63, 0x60, 0x63, 0x3e, 0x00},
	{0x03, 0x03, 0x3b, 0x67, 0x63, 0x67, 0x3b, 0x00}, {0x00, 0x00, 0x3e, 0x63, 0x7f, 0x60, 0x3f, 0x00}, {0x0e, 0x1b, 0x18, 0x3e, 0x18, 0x18, 0x18, 0x00}, {0x00, 0x00, 0x3b, 0x67, 0x67, 0x3b, 0x03, 0x3e},
	{0x60, 0x60, 0x6e, 0x73, 0x63, 0x63, 0x63, 0x00}, {0x18, 0x00, 0x38, 0x18, 0x18, 0x18, 0x3c, 0x00}, {0x06, 0x00, 0x0e, 0x06, 0x06, 0x06, 0x66, 0x3c}, {0x30, 0x30, 0x33, 0x36, 0x3c, 0x36, 0x33, 0x00},
	{0x38, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3c, 0x00}, {0x00, 0x00, 0x76, 0x6b, 0x6b, 0x6b, 0x6b, 0x00}, {0x00, 0x00, 0x6e, 0x73, 0x63, 0x63, 0x63, 0x00}, {0x00, 0x00, 0x3e, 0x63, 0x63, 0x63, 0x3e, 0x00},
	{0x00, 0x00, 0x6e, 0x73, 0x73, 0x6e, 0x60, 0x60}, {0x00, 0x00, 0x3b, 0x67, 0x67, 0x3b, 0x03, 0x03}, {0x00, 0x00, 0x6f, 0x70, 0x60, 0x60, 0x60, 0x00}, {0x00, 0x00, 0x3f, 0x60, 0x3e, 0x03, 0x7e, 0x00},
	{0x18, 0x18, 0x3e, 0x18, 0x18, 0x1b, 0x0e, 0x00}, {0x00, 0x00, 0x63, 0x63, 0x63, 0x67, 0x3b, 0x00}, {0x00, 0x00, 0x63, 0x63, 0x36, 0x36, 0x1c, 0x00}, {0x00, 0x00, 0x63, 0x6b, 0x6b, 0x77, 0x22, 0x00},
	{0x00, 0x00, 0x63, 0x36, 0x1c, 0x36, 0x63, 0x00}, {0x00, 0x00, 0x63, 0x63, 0x67, 0x3b, 0x03, 0x3e}, {0x00, 0x00, 0x3f, 0x06, 0x0c, 0x18, 0x3f, 0x00}, {0x0e, 0x18, 0x18, 0x70, 0x18, 0x18, 0x0e, 0x00},
	{0x18, 0x18, 0x18, 0x00, 0x18, 0x18, 0x18, 0x00}, {0x70, 0x18, 0x18, 0x0e, 0x18, 0x18, 0x70, 0x00}, {0x3b, 0x6e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x08, 0x1c, 0x36, 0x63, 0x63, 0x7f, 0x00, 0x00},
	{0x1e, 0x33, 0x60, 0x60, 0x33, 0x1c, 0x06, 0x1c}, {0x36, 0x00, 0x63, 0x63, 0x63, 0x67, 0x3b, 0x00}, {0x0f, 0x00, 0x3e, 0x63, 0x7f, 0x60, 0x3f, 0x00}, {0x3e, 0x41, 0x3e, 0x03, 0x3f, 0x63, 0x3f, 0x00},
	{0x36, 0x00, 0x3e, 0x03, 0x3f, 0x63, 0x3f, 0x00}, {0x78, 0x00, 0x3e, 0x03, 0x3f, 0x63, 0x3f, 0x00}, {0x1c, 0x14, 0x3e, 0x03, 0x3f, 0x63, 0x3f, 0x00}, {0x00, 0x00, 0x3e, 0x60, 0x60, 0x3e, 0x0c, 0x18},
	{0x3e, 0x41, 0x3e, 0x63, 0x7f, 0x60, 0x3f, 0x00}, {0x36, 0x00, 0x3e, 0x63, 0x7f, 0x60, 0x3f, 0x00}, {0x78, 0x00, 0x3e, 0x63, 0x7f, 0x60, 0x3f, 0x00}, {0x36, 0x00, 0x1c, 0x0c, 0x0c, 0x0c, 0x1e, 0x00},
	{0x3e, 0x63, 0x1c, 0x0c, 0x0c, 0x0c, 0x1e, 0x00}, {0x38, 0x00, 0x1c, 0x0c, 0x0c, 0x0c, 0x1e, 0x00}, {0x63, 0x1c, 0x36, 0x63, 0x7f, 0x63, 0x63, 0x00}, {0x1c, 0x36, 0x1c, 0x36, 0x63, 0x7f, 0x63, 0x00},
	{0x0f, 0x00, 0x3f, 0x30, 0x3e, 0x30, 0x3f, 0x00}, {0x00, 0x00, 0x6e, 0x1b, 0x7f, 0xd8, 0x7f, 0x00}, {0x1f, 0x3c, 0x6c, 0x7f, 0x6c, 0x6c, 0x6f, 0x00}, {0x3e, 0x41, 0x3e, 0x63, 0x63, 0x63, 0x3e, 0x00},
	{0x36, 0x00, 0x3e, 0x63, 0x63, 0x63, 0x3e, 0x00}, {0x78, 0x00, 0x3e, 0x63, 0x63, 0x63, 0x3e, 0x00}, {0x3e, 0x41, 0x00, 0x63, 0x63, 0x67, 0x3b, 0x00}, {0x78, 0x00, 0x63, 0x63, 0x63, 0x67, 0x3b, 0x00},
	{0x36, 0x00, 0x63, 0x63, 0x67, 0x3b, 0x03, 0x3e}, {0x63, 0x3e, 0x63, 0x63, 0x63, 0x63, 0x3e, 0x00}, {0x36, 0x00, 0x63, 0x63, 0x63, 0x63, 0x3e, 0x00}, {0x0c, 0x0c, 0x3f, 0x60, 0x60, 0x3f, 0x0c, 0x0c},
	{0x1c, 0x36, 0x30, 0x78, 0x30, 0x73, 0x7e, 0x00}, {0x66, 0x66, 0x3c, 0x18, 0x7e, 0x18, 0x7e, 0x18}, {0x7c, 0x66, 0x66, 0x78, 0x66, 0x6f, 0x66, 0x67}, {0x0e, 0x1b, 0x18, 0x3e, 0x18, 0x18, 0xd8, 0x70},
	{0x0f, 0x00, 0x3e, 0x03, 0x3f, 0x63, 0x3f, 0x00}, {0x0f, 0x00, 0x1c, 0x0c, 0x0c, 0x0c, 0x1e, 0x00}, {0x0f, 0x00, 0x3e, 0x63, 0x63, 0x63, 0x3e, 0x00}, {0x0f, 0x00, 0x63, 0x63, 0x63, 0x63, 0x3e, 0x00},
	{0x3b, 0x6e, 0x00, 0x6e, 0x73, 0x63, 0x63, 0x00}, {0x3b, 0x6e, 0x73, 0x7b, 0x6f, 0x67, 0x63, 0x00}, {0x3c, 0x6c, 0x6c, 0x36, 0x00, 0x7e, 0x00, 0x00}, {0x3c, 0x66, 0x66, 0x3c, 0x00, 0x7e, 0x00, 0x00},
	{0x18, 0x00, 0x18, 0x18, 0x30, 0x66, 0x3c, 0x00}, {0x00, 0x00, 0x00, 0x7e, 0x60, 0x60, 0x00, 0x00}, {0x00, 0x00, 0x00, 0x7e, 0x06, 0x06, 0x00, 0x00}, {0xc3, 0xc6, 0xcc, 0xd8, 0x36, 0x63, 0xc6, 0x0f},
	{0xc3, 0xc6, 0xcc, 0xdb, 0x37, 0x6f, 0xdf, 0x03}, {0x18, 0x00, 0x18, 0x18, 0x3c, 0x3c, 0x18, 0x00}, {0x00, 0x1b, 0x36, 0x6c, 0x36, 0x1b, 0x00, 0x00}, {0x00, 0x6c, 0x36, 0x1b, 0x36, 0x6c, 0x00, 0x00},
	{0x22, 0x88, 0x22, 0x88, 0x22, 0x88, 0x22, 0x88}, {0x55, 0xaa, 0x55, 0xaa, 0x55, 0xaa, 0x55, 0xaa}, {0xdd, 0x77, 0xdd, 0x77, 0xdd, 0x77, 0xdd, 0x77}, {0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08},
	{0x08, 0x08, 0x08, 0xf8, 0x08, 0x08, 0x08, 0x08}, {0x08, 0x08, 0xf8, 0x08, 0xf8, 0x08, 0x08, 0x08}, {0x14, 0x14, 0x14, 0xf4, 0x14, 0x14, 0x14, 0x14}, {0x00, 0x00, 0x00, 0xfc, 0x14, 0x14, 0x14, 0x14},
	{0x00, 0x00, 0xf8, 0x08, 0xf8, 0x08, 0x08, 0x08}, {0x14, 0x14, 0xf4, 0x04, 0xf4, 0x14, 0x14, 0x14}, {0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14}, {0x00, 0x00, 0xfc, 0x04, 0xf4, 0x14, 0x14, 0x14},
	{0x14, 0x14, 0xf4, 0x04, 0xfc, 0x00, 0x00, 0x00}, {0x14, 0x14, 0x14, 0xfc, 0x00, 0x00, 0x00, 0x00}, {0x08, 0x08, 0xf8, 0x08, 0xf8, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x00, 0xf8, 0x08, 0x08, 0x08, 0x08},
	{0x08, 0x08, 0x08, 0x0f, 0x00, 0x00, 0x00, 0x00}, {0x08, 0x08, 0x08, 0xff, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x00, 0xff, 0x08, 0x08, 0x08, 0x08}, {0x08, 0x08, 0x08, 0x0f, 0x08, 0x08, 0x08, 0x08},
	{0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00}, {0x08, 0x08, 0x08, 0xff, 0x08, 0x08, 0x08, 0x08}, {0x08, 0x08, 0x0f, 0x08, 0x0f, 0x08, 0x08, 0x08}, {0x14, 0x14, 0x14, 0x17, 0x14, 0x14, 0x14, 0x14},
	{0x14, 0x14, 0x17, 0x10, 0x1f, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x1f, 0x10, 0x17, 0x14, 0x14, 0x14}, {0x14, 0x14, 0xf7, 0x00, 0xff, 0x00, 0x00, 0x00}, {0x00, 0x00, 0xff, 0x00, 0xf7, 0x14, 0x14, 0x14},
	{0x14, 0x14, 0x17, 0x10, 0x17, 0x14, 0x14, 0x14}, {0x00, 0x00, 0xff, 0x00, 0xff, 0x00, 0x00, 0x00}, {0x14, 0x14, 0xf7, 0x00, 0xf7, 0x14, 0x14, 0x14}, {0x08, 0x08, 0xff, 0x00, 0xff, 0x00, 0x00, 0x00},
	{0x14, 0x14, 0x14, 0xff, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0xff, 0x00, 0xff, 0x08, 0x08, 0x08}, {0x00, 0x00, 0x00, 0xff, 0x14, 0x14, 0x14, 0x14}, {0x14, 0x14, 0x14, 0x1f, 0x00, 0x00, 0x00, 0x00},
	{0x08, 0x08, 0x0f, 0x08, 0x0f, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x0f, 0x08, 0x0f, 0x08, 0x08, 0x08}, {0x00, 0x00, 0x00, 0x1f, 0x14, 0x14, 0x14, 0x14}, {0x14, 0x14, 0x14, 0xff, 0x14, 0x14, 0x14, 0x14},
	{0x08, 0x08, 0xff, 0x08, 0xff, 0x08, 0x08, 0x08}, {0x08, 0x08, 0x08, 0xf8, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x00, 0x0f, 0x08, 0x08, 0x08, 0x08}, {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
	{0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff}, {0xf0, 0xf0, 0xf0, 0xf0, 0xf0, 0xf0, 0xf0, 0xf0}, {0x0f, 0x0f, 0x0f, 0x0f, 0x0f, 0x0f, 0x0f, 0x0f}, {0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00},
	{0x00, 0x00, 0x3b, 0x6a, 0x64, 0x6a, 0x3b, 0x00}, {0x1e, 0x33, 0x36, 0x33, 0x33, 0x33, 0x36, 0x30}, {0x7f, 0x63, 0x60, 0x60, 0x60, 0x60, 0x60, 0x00}, {0x00, 0x3f, 0x7f, 0x12, 0x12, 0x32, 0x63, 0x00},
	{0x7f, 0x31, 0x18, 0x0c, 0x18, 0x31, 0x7f, 0x00}, {0x00, 0x00, 0x3f, 0x64, 0x66, 0x66, 0x3c, 0x00}, {0x00, 0x00, 0x33, 0x33, 0x33, 0x3b, 0x36, 0x60}, {0x00, 0x00, 0x3f, 0x6c, 0x0c, 0x0c, 0x0c, 0x00},
	{0x1c, 0x08, 0x3e, 0x63, 0x3e, 0x08, 0x1c, 0x00}, {0x1c, 0x36, 0x63, 0x7f, 0x63, 0x36, 0x1c, 0x00}, {0x1c, 0x36, 0x63, 0x63, 0x36, 0x36, 0x77, 0x00}, {0x0e, 0x18, 0x0c, 0x1e, 0x33, 0x33, 0x1e, 0x00},
	{0x00, 0x76, 0xbb, 0x99, 0xdd, 0x6e, 0x00, 0x00}, {0x06, 0x04, 0x3e, 0x6b, 0x6b, 0x3e, 0x10, 0x30}, {0x0f, 0x18, 0x30, 0x3f, 0x30, 0x18, 0x0f, 0x00}, {0x3e, 0x63, 0x63, 0x63, 0x63, 0x63, 0x63, 0x00},
	{0x00, 0x7e, 0x00, 0x7e, 0x00, 0x7e, 0x00, 0x00}, {0x18, 0x18, 0x7e, 0x18, 0x18, 0x00, 0x7e, 0x00}, {0x70, 0x1c, 0x07, 0x1c, 0x70, 0x00, 0x7f, 0x00}, {0x07, 0x1c, 0x70, 0x1c, 0x07, 0x00, 0x7f, 0x00},
	{0x0e, 0x1b, 0x1b, 0x18, 0x18, 0x18, 0x18, 0x18}, {0x18, 0x18, 0x18, 0x18, 0x18, 0xd8, 0xd8, 0x70}, {0x18, 0x18, 0x00, 0x7e, 0x00, 0x18, 0x18, 0x00}, {0x00, 0x3b, 0x6e, 0x00, 0x3b, 0x6e, 0x00, 0x00},
	{0x3c, 0x66, 0x66, 0x3c, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00}, {0x0f, 0x0c, 0x0c, 0x0c, 0x6c, 0x3c, 0x1c, 0x0c},
	{0x6c, 0x76, 0x66, 0x66, 0x66, 0x00, 0x00, 0x00}, {0x3c, 0x66, 0x1c, 0x30, 0x7e, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x3c, 0x3c, 0x3c, 0x3c, 0x00, 0x00}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}
};

#define PIXEL(ch, x, y) ((demo_font_map[ch][y] >> (7 - (x))) & 1)

#define SCALE 4
#define CH_SIZE (SCALE * 8)
#define SCALE_PIXEL(ch, x, y) PIXEL(ch, (x) / SCALE, (y) / SCALE)

struct LINE {
	char * line;
	int offset;
	struct LINE * next;
};

static struct LINE * lines = NULL;
static int line_count = 0;

static void wars_add_line(const char * line)
{
	struct LINE * added = malloc(sizeof(*added));
	
	if(!added)
		return;
	
	added->line = strdup(line);
	if(!added->line)
	{
		free(added);
		return;
	}
	added->offset = -CH_SIZE * strlen(line) / 2;
	added->next = lines;
	lines = added;
	
	line_count++;
}

static int buffer_readchar(int fd, char * ch)
{
	static char ch_buffer[1024];
	static int buffer_cursor = 0, buffer_fill = 0;
	if(buffer_fill <= buffer_cursor)
	{
		buffer_cursor = 0;
		buffer_fill = read(fd, ch_buffer, sizeof(ch_buffer));
		if(buffer_fill <= 0)
			return -E_EOF;
	}
	*ch = ch_buffer[buffer_cursor++];
	return 1;
}

static int wars_init(const char * file)
{
	int i, fd = open(file, O_RDONLY);
	char line[256];
	
	if(fd < 0)
		return fd;
	
	for(;;)
	{
		for(i = 0; i != 255; i++)
		{
			if(buffer_readchar(fd, &line[i]) != 1)
			{
				if(i)
					break;
				close(fd);
				return 0;
			}
			if(line[i] == '\r')
			{
				i--;
				continue;
			}
			if(line[i] == '\n' || !line[i])
				break;
		}
		line[i] = 0;
		
		wars_add_line(line);
	}
}

static void wars_kill(void)
{
	while(lines)
	{
		struct LINE * next = lines->next;
		free(lines->line);
		free(lines);
		lines = next;
	}
}

/* The basic space text screen saver picture looks like this:
 *         +-------+              +z
 *        /         \              ^  ^
 *       /           \             | /
 *      /             \            |/
 *     /               \      <----+----> +y
 *    /                 \         /|
 *   /                   \       / |
 *  /                     \     v  v
 * +-----------------------+   +x
 * 
 * But we have to make the perspective look right. We really want the words to
 * be scrolling along a horizontal plane z = c for some reasonable c (presumably
 * near 0), so when we talk about "x" and "y" we're really talking about that
 * plane. We need to translate those coordinates into screen coordinates, which
 * are the standard computer graphics axes.
 * 
 * To make the math easier, we'll use these axes instead of those that would be
 * used for standard mathematical 3D spaces:
 *          +z
 *       ^  ^
 *       | /
 *       |/
 *  <----+----> +x
 *      /|
 *     / |
 *    v  v
 *      +y
 * 
 * Now we can see that what we want to do is to hold y constant and scroll the
 * words toward positive z. We'll end up changing the screen y coordinate and
 * adjusting the screen x coordinate based on z.
 */

/* from demo.c */
extern unsigned char demo_buffer[5 * 64000];

#define FIXED_POINT 1024

static void wars_draw_char(unsigned char ch, int x, int y, int z)
{
	/* This function used to use doubles, but it has been converted to use
	 * fixed point arithmetic. The casts to int and the variables sx and sy
	 * are left so that it is easier to see what's going on. */
	int cx, cy;
	int color = 255 - z / (2 * 2);
	if(color > 255)
		color = 255;
	else if(color <= 0)
		return;
	for(cy = 0; cy != CH_SIZE; cy++)
	{
		int sy;
		int d3_y = y * FIXED_POINT;
		int d3_z = (z + CH_SIZE - cy - 1) * FIXED_POINT;
		
		d3_z = d3_z / (200 * 2) + FIXED_POINT;
		if(d3_z <= 0)
			continue;
		/* FIXED_POINT divides out */
		d3_y /= d3_z;
		
		sy = (int) d3_y;
		if(sy < 0 || sy >= 200 * 2)
			continue;
		sy *= 320 * 2;
		
		for(cx = 0; cx != CH_SIZE; cx++)
		{
			int sx;
			int d3_x = (x + cx) * FIXED_POINT;
			
			/* FIXED_POINT divides out */
			d3_x /= d3_z;
			d3_x += 160 * 2;
			
			sx = (int) d3_x;
			
			if(sx < 0 || sx >= 320 * 2)
				continue;
			if(SCALE_PIXEL(ch, cx, cy))
			{
				int index = sy + sx;
				unsigned char * pixel = &demo_buffer[index];
				if(*pixel < color)
					*pixel = color;
			}
		}
	}
}

static void wars_display_line(char * line, int offset, int distance)
{
	for(; *line; line++)
	{
		wars_draw_char(*line, offset, 200 * 2, distance);
		offset += CH_SIZE;
	}
}

static void wars_aa_scale(void)
{
	int x, y;
	for(y = 0; y != 200; y++)
	{
		int yl = y * 320;
		for(x = 0; x != 320; x++)
		{
			int offset = (yl * 2 + x) * 2;
			int pixel = demo_buffer[offset];
			pixel += demo_buffer[offset + 1];
			pixel += demo_buffer[offset + 640];
			pixel += demo_buffer[offset + 641];
			demo_buffer[64000 * 4 + yl + x] = pixel / 4;
		}
	}
}

void wars(int argc, char * argv[])
{
	int i;
	
	if(argc < 2)
	{
		printf("Need an input file!\n");
		return;
	}
	
	for(i = 1; i != argc; i++)
	{
		int e = wars_init(argv[i]);
		if(e < 0)
		{
			printf("%s: %e\n", argv[i], e);
			return;
		}
	}
	
	sys_vga_set_mode_320(0xA0000);
	
	i = -CH_SIZE * line_count + SCALE;
	while(getchar_nb() == -1)
	{
		const int frame_end = env->env_jiffies + 4 * HZ / 100;
		int offset = 0, draw = 0;
		struct LINE * line;
		
		memset(demo_buffer, 0, 64000 * 4);
		
		for(line = lines; line; line = line->next)
		{
			int distance = offset + i;
			if(-CH_SIZE <= distance && distance < 512 * 2)
			{
				wars_display_line(line->line, line->offset, distance);
				draw++;
			}
			offset += CH_SIZE;
		}
		
		i += SCALE;
		
		wars_aa_scale();
		memcpy((void *) 0xA0000, &demo_buffer[64000 * 4], 64000);
		while(frame_end - env->env_jiffies > 0)
			sys_yield();
		if(!draw)
			break;
	}
	
	sys_vga_set_mode_text();
	
	wars_kill();
}
