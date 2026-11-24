#pragma once
#include "optical_wire.h"
#include "receiver.h"
#include <cstdio>
#include <cinttypes>

namespace optical_uart {
// The only ingress to typed Receiver methods for BINARY1 is after full CRC,
// length, profile, identity and sequence validation. TEXT2 is a separate mode.
class BinaryTransport {
 public:
  explicit BinaryTransport(Receiver &receiver): core(receiver) {}
  Receiver &core;
  uint32_t crc_errors=0,framing_errors=0,sequence_errors=0,frame_gaps=0,
           protocol_errors=0,unknown_frames=0,received_frames=0;
  uint8_t last_type=0;
  char raw_text[192]{},meter_text[256]{},info_text[512]{},status_text[512]{};

  void expire(uint32_t now) {
    if (length_ && now-last_byte_>1000) {
      length_=0; dropping_=true; fail_(framing_errors);
    }
  }
  // Returns a known type on successful semantic acceptance, 0 otherwise.
  uint8_t push(uint8_t byte,uint32_t now) {
    last_byte_=now;
    if (!byte) {
      if (dropping_ || !length_) { dropping_=false; length_=0; return 0; }
      size_t length=length_; length_=0;
      return packet_(encoded_,length,now);
    }
    if (dropping_) return 0;
    if (length_==sizeof(encoded_)) { length_=0; dropping_=true; fail_(framing_errors); return 0; }
    encoded_[length_++]=byte; return 0;
  }
 private:
  uint8_t encoded_[OW_MAX_ENCODED]{};
  size_t length_=0;
  uint32_t last_byte_=0,sequence_=0,boot_=0;
  bool dropping_=false,have_sequence_=false;
  char device_[25]{};
  void fail_(uint32_t &counter) { ++counter; core.malformed(); }
  uint8_t packet_(const uint8_t *bytes,size_t length,uint32_t now) {
    uint8_t p[OW_MAX_DECODED];
    size_t n=ow_cobs_decode(bytes,length,p,sizeof(p));
    if (n<OW_RAW_HEADER_SIZE+2) { fail_(framing_errors); return 0; }
    if (ow_crc16(p,n-2)!=ow_get16(p+n-2)) { fail_(crc_errors); return 0; }
    uint16_t payload_length=ow_get16(p+4);
    size_t header=ow_header_size(p[1]);
    if (n!=header+(size_t)payload_length+2 || p[0]!=OW_VERSION || (p[3]&~31U)) {
      fail_(protocol_errors); return 0;
    }
    char device[25]{};
    uint32_t boot=ow_get32(p+10),seq=ow_get32(p+6);
    if (!boot) { fail_(protocol_errors); core.invalidate_session(); return 0; }
    bool raw=p[1]==OW_RAW;
    if (raw) {
      // RAW cannot establish identity. Boot changes invalidate the old session;
      // same-boot source replacement is detected by full-identity frames before
      // any new checkpoint can be committed. RAW remains provisional diagnosis.
      if (!have_sequence_ || boot!=boot_ || !core.profile_ok) {
        core.invalidate_session(); have_sequence_=false; return 0;
      }
      std::strcpy(device,device_);
    } else {
      std::snprintf(device,sizeof(device),"%08" PRIX32 "%08" PRIX32 "%08" PRIX32,
                    ow_get32(p+22),ow_get32(p+26),ow_get32(p+30));
      if (!have_sequence_ || boot!=boot_ || std::strcmp(device_,device)) {
        core.invalidate_session(); have_sequence_=false;
        boot_=boot; std::strcpy(device_,device);
      }
    }
    if (have_sequence_) {
      uint32_t ds=seq-sequence_;
      if (!ds || ds>=0x80000000U) { fail_(sequence_errors); return 0; }
      if (ds>1) frame_gaps+=ds-1;
    }
    sequence_=seq; have_sequence_=true; ++received_frames;
    uint8_t type=p[1];
    unsigned expected=type==OW_INFO?OW_INFO_SIZE:type==OW_RAW?OW_RAW_SIZE:
      type==OW_METER?OW_METER_SIZE:type==OW_STATUS?OW_STATUS_SIZE:0;
    // Unknown future messages are bounded, CRC-checked, sequenced and skipped.
    if (!expected || p[2]!=1) {
      ++unknown_frames;
      if (type==OW_INFO) core.invalidate_session();
      return 0;
    }
    if (payload_length!=expected) { fail_(protocol_errors); return 0; }
    const uint8_t *v=p+header;
    uint32_t sample=ow_get32(p+14),time=ow_get32(p+18);
    bool accepted=false;
    if (type==OW_INFO) {
      bool compatible=v[0]==1 && v[1]==0 && v[2]==0 && v[3]==1 && v[4]==1 &&
        v[5]==12 && v[6]==6 && v[7]==1 && ow_get32(v+8)==1000000 &&
        ow_get16(v+12)==10 && ow_get16(v+14)==50 && ow_get16(v+16)==50 &&
        ow_get16(v+18)==40 && v[20]==2 && v[21]==1 && v[22]==0 && v[23]==1 &&
        ow_get32(v+24)==48000000 && ow_get32(v+28)==12000000;
      accepted=core.accept_identity(device,boot,compatible);
      std::snprintf(info_text,sizeof(info_text),
        "INFO,protocol=BINARY1,fw=%u.%u.%u,hw=%u,local=%u,device=%s,boot=%" PRIu32
        ",adc_bits=%u,steps_per_cycle=%u,reverse=%u,liters_per_cycle_ul=%" PRIu32
        ",target_hz=%u,led_off_wait_ms=%u,led_on_wait_ms=%u,minimum_span=%u,learning_cycles=%u"
        ",polarity=%u,count_store=%u,cal_store=%u,cpu_hz=%" PRIu32 ",adc_hz=%" PRIu32
        ",crc=CCITT-FALSE,profile_ok=%u,sequence=%" PRIu32,
        v[0],v[1],v[2],v[3],v[4],device,boot,v[5],v[6],v[7],ow_get32(v+8),
        ow_get16(v+12),ow_get16(v+14),ow_get16(v+16),ow_get16(v+18),v[20],v[21],v[22],v[23],
        ow_get32(v+24),ow_get32(v+28),compatible?1U:0U,seq);
    } else {
      // Never accept readings before the exact producer profile is known.
      if (!core.profile_ok || core.boot_id()!=boot || std::strcmp(core.device_id(),device)) return 0;
      if (type==OW_RAW) {
        Raw r; r.sample=sample; r.time_ms=time; r.flags=p[3];
        for (unsigned i=0;i<3;++i) { r.off[i]=ow_get16(v+i*2); r.on[i]=ow_get16(v+6+i*2); }
        accepted=core.accept_raw(r,now);
        if (accepted) std::snprintf(raw_text,sizeof(raw_text),
          "RAW,%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%u,%u,%u,%u,%u,%u,boot=%" PRIu32 ",sequence=%" PRIu32,
          r.sample,r.time_ms,r.flags,r.off[0],r.off[1],r.off[2],r.on[0],r.on[1],r.on[2],boot,seq);
      } else if (type==OW_METER) {
        Meter m; m.sample=sample; m.time_ms=time; m.steps=ow_get_i64(v);
        m.rejected=ow_get32(v+8); m.invalid=ow_get32(v+12); m.dark_rail=ow_get32(v+16);
        if (v[20]>2 || (v[21]>5 && v[21]!=255) || ow_get16(v+22)) { fail_(protocol_errors); return 0; }
        m.phase=v[21]==255?-1:static_cast<int>(v[21]);
        std::strcpy(m.state,v[20]==1?"READY":v[20]==2?"SIGNAL":"LEARNING");
        // An identity-bearing METER must refer to the actual preceding RAW time.
        if (core.raw.sample!=sample || core.raw.time_ms!=time) { fail_(protocol_errors); return 0; }
        accepted=core.accept_meter(m,now);
        if (accepted) std::snprintf(meter_text,sizeof(meter_text),
          "METER,sample_id=%" PRIu32 ",state=%s,phase=%d,steps=%" PRId64
          ",rejected=%" PRIu32 ",invalid=%" PRIu32 ",dark_rail=%" PRIu32 ",boot=%" PRIu32 ",sequence=%" PRIu32,
          sample,m.state,m.phase,m.steps,m.rejected,m.invalid,m.dark_rail,boot,seq);
      } else {
        if ((v[40]&~31U) || v[41] || v[42] || v[43]) { fail_(protocol_errors); return 0; }
        Status s; s.boot=boot; s.sample=sample; s.uptime=ow_get32(v);
        s.adc_errors=ow_get32(v+4); s.uart_errors=ow_get32(v+8);
        s.format_errors=ow_get32(v+12); s.store_errors=ow_get32(v+16);
        for (unsigned i=0;i<3;++i) {
          s.low[i]=ow_get16(v+28+i*2); s.high[i]=ow_get16(v+34+i*2);
          if (s.low[i]>4095 || s.high[i]>4095) { fail_(protocol_errors); return 0; }
        }
        s.sample_valid=v[40]&1; s.adc_ok=v[40]&2; s.boot_ok=v[40]&4;
        s.saved=v[40]&8; s.restored=v[40]&16;
        if (s.uptime-time>=0x80000000U) { fail_(protocol_errors); return 0; }
        accepted=core.accept_status(s,now);
        if (accepted) std::snprintf(status_text,sizeof(status_text),
          "STATUS,boot=%" PRIu32 ",uptime_ms=%" PRIu32 ",sample_id=%" PRIu32
          ",adc_errors=%" PRIu32 ",uart_errors=%" PRIu32 ",format_errors=%" PRIu32
          ",store_errors=%" PRIu32 ",sample_valid=%u,calibration_ok=%u,boot_ok=%u"
          ",low=%u|%u|%u,high=%u|%u|%u,meter_ms=%" PRIu32 ",meter_max_ms=%" PRIu32
          ",cal_saved=%u,cal_restored=%u,sequence=%" PRIu32,
          boot,s.uptime,sample,s.adc_errors,s.uart_errors,s.format_errors,s.store_errors,
          s.sample_valid?1U:0U,s.adc_ok?1U:0U,s.boot_ok?1U:0U,
          s.low[0],s.low[1],s.low[2],s.high[0],s.high[1],s.high[2],ow_get32(v+20),ow_get32(v+24),
          s.saved?1U:0U,s.restored?1U:0U,seq);
      }
    }
    if (!accepted) { fail_(protocol_errors); return 0; }
    last_type=type; return type;
  }
};
}  // namespace optical_uart
