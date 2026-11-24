#ifndef OPTICAL_WIRE_H
#define OPTICAL_WIRE_H
/* BINARY1 / firmware 1.0.0 wire codec, shared verbatim by STM32 C and ESPHome C++.
 * Explicit little-endian fields; no packed structs or dynamic allocation.
 * CRC-16/CCITT-FALSE over header + payload, then COBS, then 0x00.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define OW_VERSION 1U
#define OW_HEADER_SIZE 34U
#define OW_RAW_HEADER_SIZE 22U
#define OW_MAX_DECODED 128U
#define OW_MAX_ENCODED 129U
#define OW_MAX_WIRE 130U
#define OW_INFO 0x01U
#define OW_RAW 0x10U
#define OW_METER 0x20U
#define OW_STATUS 0x30U
#define OW_INFO_SIZE 32U
#define OW_RAW_SIZE 12U
#define OW_METER_SIZE 24U
#define OW_STATUS_SIZE 44U

static inline void ow_put16(uint8_t *p,uint16_t v) {
  p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8);
}
static inline void ow_put32(uint8_t *p,uint32_t v) {
  for (unsigned i=0;i<4;++i) p[i]=(uint8_t)(v>>(i*8));
}
static inline void ow_put64(uint8_t *p,uint64_t v) {
  for (unsigned i=0;i<8;++i) p[i]=(uint8_t)(v>>(i*8));
}
static inline uint16_t ow_get16(const uint8_t *p) {
  return (uint16_t)((uint16_t)p[0]|((uint16_t)p[1]<<8));
}
static inline uint32_t ow_get32(const uint8_t *p) {
  uint32_t v=0; for (unsigned i=0;i<4;++i) v|=(uint32_t)p[i]<<(i*8); return v;
}
static inline uint64_t ow_get64(const uint8_t *p) {
  uint64_t v=0; for (unsigned i=0;i<8;++i) v|=(uint64_t)p[i]<<(i*8); return v;
}
static inline int64_t ow_get_i64(const uint8_t *p) {
  uint64_t v=ow_get64(p);
  return (v>>63) ? -1-(int64_t)(~v) : (int64_t)v;
}
static inline uint16_t ow_crc16(const uint8_t *p,size_t n) {
  uint16_t crc=0xFFFFU;
  while (n--) {
    crc^=(uint16_t)*p++<<8;
    for (unsigned b=0;b<8;++b) crc=(uint16_t)((crc<<1)^((crc&0x8000U)?0x1021U:0U));
  }
  return crc;
}
/* Valid only for bounded packets <=128 bytes; no 254-byte COBS run needed. */
static inline size_t ow_cobs_encode(const uint8_t *src,size_t n,uint8_t *dst,size_t cap) {
  if (n>OW_MAX_DECODED || cap<n+1) return 0;
  size_t code_at=0,out=1; uint8_t code=1;
  for (size_t i=0;i<n;++i) {
    if (src[i]==0) { dst[code_at]=code; code_at=out++; code=1; }
    else { dst[out++]=src[i]; ++code; }
  }
  dst[code_at]=code; return out;
}
static inline size_t ow_cobs_decode(const uint8_t *src,size_t n,uint8_t *dst,size_t cap) {
  if (!n || n>OW_MAX_ENCODED) return 0;
  size_t in=0,out=0;
  while (in<n) {
    unsigned code=src[in++];
    if (!code || code==255 || (size_t)(code-1)>n-in) return 0;
    for (unsigned j=1;j<code;++j) {
      if (!src[in] || out==cap) return 0;
      dst[out++]=src[in++];
    }
    if (in<n) { if (out==cap) return 0; dst[out++]=0; }
  }
  return out;
}
static inline size_t ow_header_size(uint8_t type) {
  return type==OW_RAW ? OW_RAW_HEADER_SIZE : OW_HEADER_SIZE;
}
/* RAW carries boot/session fields; other types also carry the full UID. */
static inline size_t ow_encode(uint8_t type,uint8_t flags,uint32_t sequence,
    uint32_t boot,uint32_t sample,uint32_t time_ms,const uint32_t uid[3],
    const uint8_t *payload,uint16_t length,uint8_t *wire,size_t capacity) {
  uint8_t packet[OW_MAX_DECODED];
  size_t header=ow_header_size(type);
  size_t n=header+(size_t)length+2;
  if (n>sizeof(packet) || capacity<n+2 || (!payload && length)) return 0;
  packet[0]=OW_VERSION; packet[1]=type; packet[2]=1; packet[3]=flags;
  ow_put16(packet+4,length); ow_put32(packet+6,sequence); ow_put32(packet+10,boot);
  ow_put32(packet+14,sample); ow_put32(packet+18,time_ms);
  if (type!=OW_RAW) {
    for (unsigned i=0;i<3;++i) ow_put32(packet+22+i*4,uid[i]);
  }
  if (length) memcpy(packet+header,payload,length);
  ow_put16(packet+n-2,ow_crc16(packet,n-2));
  size_t encoded=ow_cobs_encode(packet,n,wire,capacity-1);
  if (!encoded) return 0;
  wire[encoded]=0; return encoded+1;
}
#endif
