// SPDX-License-Identifier: GPL-3.0-or-later
//
// Link stub for the console's libSceVideodec2 (the hardware video decoder):
// the payload SDK has none. Built as libSceVideodec2.so with SONAME
// libSceVideodec2.sprx; the native converter turns each call into an import
// of the console's module by NID, and the UI loads that module (sysmodule
// 207) before the first call (ps5_hwdec.cpp). Only the names matter; these
// bodies never run. Names as VLC for PS5 imports them
// (ps5/stubs/videodec2_link_stub.c).

int sceVideodec2QueryComputeMemoryInfo(void* info) {
  (void)info;
  return -1;
}
int sceVideodec2AllocateComputeQueue(const void* config, const void* memory, void** queue) {
  (void)config;
  (void)memory;
  (void)queue;
  return -1;
}
int sceVideodec2ReleaseComputeQueue(void* queue) {
  (void)queue;
  return -1;
}
int sceVideodec2QueryDecoderMemoryInfo(const void* config, void* memory) {
  (void)config;
  (void)memory;
  return -1;
}
int sceVideodec2CreateDecoder(const void* config, const void* memory, void** decoder) {
  (void)config;
  (void)memory;
  (void)decoder;
  return -1;
}
int sceVideodec2DeleteDecoder(void* decoder) {
  (void)decoder;
  return -1;
}
int sceVideodec2Decode(void* decoder, const void* input, void* frame, void* output) {
  (void)decoder;
  (void)input;
  (void)frame;
  (void)output;
  return -1;
}
int sceVideodec2Flush(void* decoder, void* frame, void* output) {
  (void)decoder;
  (void)frame;
  (void)output;
  return -1;
}
int sceVideodec2Reset(void* decoder) {
  (void)decoder;
  return -1;
}
