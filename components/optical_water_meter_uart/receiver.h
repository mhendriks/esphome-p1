#pragma once
// TEXT2 / STM32 0.5.0 receiver. Independent of ESPHome for replay testing.
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cmath>
#include <limits>

namespace optical_uart {
struct Checkpoint {
  uint32_t magic=0x55415231, version=1;
  double offset_m3=0;
  int64_t total_steps=0, trial_start=0, source_steps=0;
  uint32_t boot=0, sample=0, time_ms=0, rejected=0, invalid=0;
  char device[25]{};
  bool anchored=false;
};
struct Raw {
  uint32_t sample=0, time_ms=0, flags=0;
  uint16_t off[3]{}, on[3]{};
  bool valid=false;
};
struct Meter {
  uint32_t sample=0, time_ms=0, rejected=0, invalid=0, dark_rail=0;
  int64_t steps=0;
  int phase=-1;
  bool ready=false;
  char state[9]="WAITING";
};
struct Status {
  uint32_t boot=0, uptime=0, sample=0, adc_errors=0, uart_errors=0,
           format_errors=0, store_errors=0;
  uint16_t low[3]{}, high[3]{};
  bool sample_valid=false, adc_ok=false, boot_ok=false, saved=false, restored=false;
};

class LineBuffer {
 public:
  char data[512]{};
  // 1 = complete line, -1 = newly detected malformed line, 0 = incomplete/discarded.
  int push(uint8_t b,uint32_t now) {
    last_byte_=now;
    if (b=='\n') {
      bool good=!dropping_ && length_;
      if (good) data[length_]=0;
      length_=0; dropping_=false; cr_=false;
      return good?1:0;
    }
    if (dropping_) return 0;
    if (cr_ || (b!='\r' && (b<32 || b>126)) || length_==sizeof(data)-1) {
      length_=0; dropping_=true; cr_=false; return -1;
    }
    if (b=='\r') cr_=true;
    else data[length_++]=static_cast<char>(b);
    return 0;
  }
  bool expire(uint32_t now) {
    if ((length_ || cr_) && now-last_byte_>1000) {
      length_=0; cr_=false; dropping_=true; return true;
    }
    return false;
  }
 private:
  uint32_t last_byte_=0;
  size_t length_=0;
  bool dropping_=false,cr_=false;
};

// Strict decimal parsing: no whitespace, signs on unsigned values, or trailing junk.
inline bool number(const char *s, uint64_t limit, uint64_t &out) {
  if (!s || !*s) return false;
  uint64_t v=0;
  for (; *s; ++s) {
    if (*s<'0' || *s>'9') return false;
    unsigned d=*s-'0';
    if (d>limit || v>(limit-d)/10) return false;
    v=v*10+d;
  }
  out=v; return true;
}
class Fields {
 public:
  char *tokens[40]{};
  unsigned count=0;
  bool split(char *line) {
    tokens[count++]=line;
    for (char *p=line; *p; ++p) if (*p==',') {
      *p=0;
      if (count==40 || !p[1]) return false;
      tokens[count++]=p+1;
    }
    return true;
  }
  bool kv() {
    for (unsigned i=1;i<count;++i) {
      char *eq=std::strchr(tokens[i],'=');
      if (!eq || eq==tokens[i] || !eq[1]) return false;
      *eq=0;
      for (unsigned j=1;j<i;++j) if (!std::strcmp(tokens[j],tokens[i])) return false;
    }
    return true;
  }
  const char *get(const char *key) const {
    for (unsigned i=1;i<count;++i) if (!std::strcmp(tokens[i],key))
      return tokens[i]+std::strlen(tokens[i])+1;
    return nullptr;
  }
  bool eq(const char *key, const char *value) const {
    const char *v=get(key); return v && !std::strcmp(v,value);
  }
  bool u(const char *key,uint32_t &v) const {
    uint64_t n; if (!number(get(key),UINT32_MAX,n)) return false;
    v=static_cast<uint32_t>(n); return true;
  }
  bool bit(const char *key,bool &v) const {
    uint32_t n; if (!u(key,n) || n>1) return false;
    v=n!=0; return true;
  }
  bool steps(int64_t &v) const {
    const char *s=get("steps"); if (!s) return false;
    bool neg=*s=='-'; if (neg) ++s;
    uint64_t n;
    if (!number(s,static_cast<uint64_t>(INT64_MAX)+(neg?1ULL:0ULL),n)) return false;
    v=neg ? (n==static_cast<uint64_t>(INT64_MAX)+1ULL ? INT64_MIN : -static_cast<int64_t>(n)) : static_cast<int64_t>(n);
    return true;
  }
  bool triple(const char *key,uint16_t v[3]) const {
    const char *s=get(key); if (!s) return false;
    for (unsigned i=0;i<3;++i) {
      char part[6]{}; unsigned n=0;
      while (*s && *s!='|') { if (n==5) return false; part[n++]=*s++; }
      uint64_t x; if (!number(part,4095,x)) return false;
      v[i]=static_cast<uint16_t>(x);
      if (i<2) { if (*s++!='|') return false; } else if (*s) return false;
    }
    return true;
  }
};

class Receiver {
 public:
  Checkpoint saved{};
  Raw raw{};
  Meter meter{};
  Status status{};
  bool interrupted=true, profile_ok=false;
  uint32_t parse_errors=0, raw_gaps=0;
  // True after ingesting a complete matching STATUS checkpoint (about every 5s).
  bool committed=false;
  const char *device_id() const { return device_; }
  uint32_t boot_id() const { return info_boot_; }
  bool raw_online(uint32_t now) const {
    return profile_ok && have_status_ && have_raw_ && status.boot==info_boot_ &&
      status.boot_ok && status.adc_ok && now-last_status_<7000 && now-last_raw_<2000;
  }

  bool restore(const Checkpoint &p) {
    if (p.magic!=0x55415231 || p.version!=1 || !std::isfinite(p.offset_m3) ||
        std::fabs(p.offset_m3)>1000000 || p.device[24]!=0 ||
        std::fabs(static_cast<double>(p.total_steps))>1e12 ||
        std::fabs(static_cast<double>(p.trial_start))>1e12 ||
        (p.anchored && (!device_valid(p.device) || !p.boot))) return false;
    saved=p; interrupted=true; return true;
  }
  double meter_m3() const { return saved.offset_m3+static_cast<double>(saved.total_steps)/6000.0; }
  double trial_liters() const { return (static_cast<double>(saved.total_steps)-static_cast<double>(saved.trial_start))/6.0; }
  bool online(uint32_t now) const {
    return profile_ok && have_status_ && have_raw_ && have_meter_ &&
      now-last_status_<7000 && now-last_raw_<2000 && now-last_meter_<2500;
  }
  bool valid(uint32_t now) const {
    return online(now) && meter.ready && raw.valid && status.sample_valid && status.adc_ok && status.boot_ok &&
      status.boot==info_boot_ && saved.anchored && confirmed_ && saved.boot==status.boot &&
      !std::strcmp(saved.device,device_);
  }
  void tick(uint32_t now) {
    if (was_online_ && !online(now)) interrupted=true;
    was_online_=online(now);
  }
  bool set_meter(double m3,uint32_t now) {
    if (!std::isfinite(m3) || m3<0 || m3>99999 || !valid(now)) return false;
    saved.offset_m3=m3-static_cast<double>(saved.total_steps)/6000.0;
    interrupted=false; return true;
  }
  void reset_trial() { saved.trial_start=saved.total_steps; }
  void malformed() { ++parse_errors; interrupted=true; pending_=false; }
  bool ingest(const char *line,uint32_t now) {
    committed=false;
    size_t n=std::strlen(line);
    if (n==0 || n>=512) { malformed(); return false; }
    char buffer[512]; std::memcpy(buffer,line,n+1);
    Fields f;
    if (!f.split(buffer)) { malformed(); return false; }
    bool ok=false;
    if (!std::strcmp(f.tokens[0],"RAW")) ok=read_raw_(f,now);
    else if (f.kv()) {
      if (!std::strcmp(f.tokens[0],"INFO")) ok=read_info_(f);
      else if (!std::strcmp(f.tokens[0],"METER")) ok=read_meter_(f,now);
      else if (!std::strcmp(f.tokens[0],"STATUS")) ok=read_status_(f,now);
    }
    if (!ok) malformed();
    return ok;
  }
  void invalidate_session() {
    profile_ok=false; have_raw_=false; have_status_=false; have_meter_=false;
    pending_=false; confirmed_=false; interrupted=true; committed=false;
  }
  bool accept_identity(const char *device,uint32_t boot,bool compatible) {
    committed=false;
    if (!device_valid(device) || !boot) return false;
    if (!compatible) { invalidate_session(); return true; }
    if (info_boot_ && (info_boot_!=boot || std::strcmp(device_,device))) invalidate_session();
    std::strcpy(device_,device); info_boot_=boot; profile_ok=true;
    return true;
  }
  bool accept_raw(Raw value,uint32_t now) {
    committed=false;
    for (unsigned i=0;i<3;++i) if (value.off[i]>4095 || value.on[i]>4095) return false;
    if (have_raw_) {
      uint32_t ds=value.sample-raw.sample,dt=value.time_ms-raw.time_ms;
      if (ds==0 || ds>=0x80000000U || dt>=0x80000000U) {
        profile_ok=false; have_status_=false; pending_=false; interrupted=true; confirmed_=false;
      } else if (ds>1) raw_gaps+=ds-1;
    }
    raw=value; raw.valid=(raw.flags&~4U)==0;
    for (unsigned i=0;i<3;++i)
      if (!raw.off[i] || !raw.on[i] || raw.on[i]>=4095 || raw.on[i]>raw.off[i]) raw.valid=false;
    if (!raw.valid) interrupted=true;
    have_raw_=true; last_raw_=now; return true;
  }
  bool accept_meter(Meter m,uint32_t now) {
    committed=false;
    if (std::fabs(static_cast<double>(m.steps))>1e12 || m.phase < -1 || m.phase>5 ||
        (std::strcmp(m.state,"READY") && std::strcmp(m.state,"LEARNING") && std::strcmp(m.state,"SIGNAL"))) return false;
    m.ready=!std::strcmp(m.state,"READY");
    if (m.ready && m.phase<0) return false;
    meter=m; have_meter_=true; last_meter_=now;
    if (!m.ready) interrupted=true;
    if (have_raw_ && m.sample==raw.sample && now-last_raw_<2000) {
      m.time_ms=raw.time_ms; pending_meter_=m; pending_=true;
    } else { pending_=false; interrupted=true; }
    meter=m; return true;
  }
  bool accept_status(const Status &s,uint32_t now) {
    committed=false;
    if (have_status_ && (s.adc_errors!=status.adc_errors || s.uart_errors!=status.uart_errors ||
        s.format_errors!=status.format_errors || s.store_errors!=status.store_errors)) interrupted=true;
    status=s; have_status_=true; last_status_=now;
    if (!s.boot_ok || !s.adc_ok || !s.sample_valid || s.store_errors) interrupted=true;
    if (!profile_ok || info_boot_!=s.boot) { interrupted=true; pending_=false; confirmed_=false; return true; }
    if (!pending_ || now-last_meter_>=2500 || s.sample-pending_meter_.sample>=0x80000000U ||
        s.uptime-pending_meter_.time_ms>=0x80000000U || !s.boot_ok) return true;
    const Meter &m=pending_meter_;
    bool same=saved.anchored && saved.boot==s.boot && !std::strcmp(saved.device,device_);
    if (same) {
      if (m.rejected!=saved.rejected || m.invalid!=saved.invalid) interrupted=true;
      uint32_t ds=m.sample-saved.sample,dt=m.time_ms-saved.time_ms;
      // Bound both integer arithmetic and plausible motion before subtraction.
      long double delta=static_cast<long double>(m.steps)-saved.source_steps;
      bool plausible=ds<0x80000000U && dt<0x80000000U &&
        std::fabs(delta)<=static_cast<long double>(ds)*3 &&
        std::fabs(delta)<=static_cast<long double>(dt)*0.03L+3 &&
        std::fabs(static_cast<long double>(saved.total_steps)+delta)<=1e12;
      if (plausible && m.ready && s.adc_ok && s.sample_valid) {
        saved.total_steps+=static_cast<int64_t>(delta);
      } else interrupted=true;
    } else interrupted=true;
    saved.boot=s.boot; saved.source_steps=m.steps; saved.sample=m.sample;
    saved.time_ms=m.time_ms; saved.rejected=m.rejected; saved.invalid=m.invalid;
    std::strcpy(saved.device,device_); saved.anchored=true;
    pending_=false; committed=true; confirmed_=true; return true;
  }
 private:
  char device_[25]{};
  uint32_t info_boot_=0,last_raw_=0,last_status_=0,last_meter_=0;
  bool have_raw_=false,have_status_=false,have_meter_=false,pending_=false,was_online_=false,confirmed_=false;
  Meter pending_meter_{};
  static bool device_valid(const char *s) {
    if (!s || std::strlen(s)!=24) return false;
    for (unsigned i=0;i<24;++i) if (!((s[i]>='0'&&s[i]<='9')||(s[i]>='A'&&s[i]<='F'))) return false;
    return true;
  }
  bool read_info_(const Fields &f) {
    uint32_t boot;
    if (!f.u("boot",boot) || !boot || !device_valid(f.get("device"))) return false;
    bool match=f.eq("protocol","TEXT2") && f.eq("fw","0.5.0") && f.eq("hw","temt6000-proto1") &&
      f.eq("adc_bits","12") && f.eq("polarity","off-on") && f.eq("local","beta1") &&
      f.eq("steps_per_cycle","6") && f.eq("liters_per_cycle","1") && f.eq("reverse","1") &&
      f.eq("target_hz","10") && f.eq("count_store","ram") && f.eq("crc","none");
    return accept_identity(f.get("device"),boot,match);
  }
  bool read_raw_(const Fields &f,uint32_t now) {
    if (f.count!=10) return false;
    uint32_t v[9]{};
    for (unsigned i=0;i<9;++i) {
      uint64_t x; if (!number(f.tokens[i+1],i<3?UINT32_MAX:4095,x)) return false;
      v[i]=static_cast<uint32_t>(x);
    }
    Raw value; value.sample=v[0]; value.time_ms=v[1]; value.flags=v[2];
    for (unsigned i=0;i<3;++i) { value.off[i]=v[3+i]; value.on[i]=v[6+i]; }
    return accept_raw(value,now);
  }
  bool read_meter_(const Fields &f,uint32_t now) {
    Meter m;
    if (!f.u("sample_id",m.sample) || !f.steps(m.steps) || std::fabs(static_cast<double>(m.steps))>1e12 || !f.u("rejected",m.rejected) ||
        !f.u("invalid",m.invalid) || !f.u("dark_rail",m.dark_rail)) return false;
    if (f.eq("phase","-1")) m.phase=-1;
    else { uint32_t p; if (!f.u("phase",p) || p>5) return false; m.phase=static_cast<int>(p); }
    m.ready=f.eq("state","READY");
    if (!m.ready && !f.eq("state","LEARNING") && !f.eq("state","SIGNAL")) return false;
    if (m.ready && m.phase<0) return false;
    std::strcpy(m.state,f.get("state"));
    return accept_meter(m,now);
  }
  bool read_status_(const Fields &f,uint32_t now) {
    Status s;
    if (!f.u("boot",s.boot) || !f.u("uptime_ms",s.uptime) || !f.u("sample_id",s.sample) ||
        !f.u("adc_errors",s.adc_errors) || !f.u("uart_errors",s.uart_errors) ||
        !f.u("format_errors",s.format_errors) || !f.u("store_errors",s.store_errors) ||
        !f.bit("sample_valid",s.sample_valid) || !f.bit("calibration_ok",s.adc_ok) ||
        !f.bit("boot_ok",s.boot_ok) || !f.bit("cal_saved",s.saved) || !f.bit("cal_restored",s.restored) ||
        !f.triple("low",s.low) || !f.triple("high",s.high)) return false;
    return accept_status(s,now);
  }
};
}  // namespace optical_uart
