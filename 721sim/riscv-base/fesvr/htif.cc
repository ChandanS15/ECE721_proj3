// See LICENSE for license details.

#include "htif.h"
#include "rfb.h"
#include "elfloader.h"
#include <algorithm>
#include <assert.h>
#include <vector>
#include <queue>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <stdio.h>
#include <limits.h>
#include <unistd.h>
#include <signal.h>
#include <inttypes.h>
#include "syscall_bypass.h"

/* Attempt to determine the execution prefix automatically.  autoconf
 * sets PREFIX, and pconfigure sets __PCONFIGURE__PREFIX. */
#if !defined(PREFIX) && defined(__PCONFIGURE__PREFIX)
# define PREFIX __PCONFIGURE__PREFIX
#endif

#ifndef TARGET_ARCH
# define TARGET_ARCH "riscv64-unknown-elf"
#endif

#ifndef TARGET_DIR
# define TARGET_DIR "/" TARGET_ARCH "/bin/"
#endif

static volatile bool signal_exit = false;
static void handle_signal(int sig)
{
  if (sig == SIGABRT || signal_exit) // someone set up us the bomb!
    exit(-1);
  signal_exit = true;
  signal(sig, &handle_signal);
}

void htif_t::set_chroot(const char* where)
{
  char buf1[PATH_MAX], buf2[PATH_MAX];

  if (getcwd(buf1, sizeof(buf1)) == NULL
      || chdir(where) != 0
      || getcwd(buf2, sizeof(buf2)) == NULL
      || chdir(buf1) != 0)
  {
    printf("could not chroot to [%s]\n", where);
    exit(-1);
  }

  chroot = buf2;
}

htif_t::htif_t(const std::vector<std::string>& args)
  : exitcode(0), mem(this), seqno(1), started(false), stopped(false),
    _mem_mb(0), _num_cores(0), sig_addr(0), sig_len(0)
{
  signal(SIGINT, &handle_signal);
  signal(SIGTERM, &handle_signal);
  signal(SIGABRT, &handle_signal); // we still want to call static destructors
  syscall_proxy = bypassed_syscall_device_auto_factory::make_syscall_device(this);

  size_t i;
  for (i = 0; i < args.size(); i++)
    if (args[i].length() && args[i][0] != '-' && args[i][0] != '+')
      break;

  hargs.insert(hargs.begin(), args.begin(), args.begin() + i);
  targs.insert(targs.begin(), args.begin() + i, args.end());
  std::string target_init_cwd;

  for (auto& arg : hargs)
  {
    if (arg == "+rfb")
      dynamic_devices.push_back(new rfb_t);
    else if (arg.find("+strace=") == 0)
      syscall_proxy->enable_strace(arg.c_str() + strlen("+strace="));
    else if (arg.find("+std-dump=") == 0)
    {
      std::string dump_file_name(arg.c_str() + strlen("+std-dump="));
      syscall_proxy->dump_std_out_err(
        (dump_file_name+".stdout").c_str(),
        (dump_file_name+".stderr").c_str()
      );
    }
    else if (arg.find("+final-state-dump=") == 0)
    {
      this->set_state_dump_path(arg.c_str() + strlen("+final-state-dump="));
    }
    else if (arg.find("+rfb=") == 0)
      dynamic_devices.push_back(new rfb_t(atoi(arg.c_str() + strlen("+rfb="))));
    else if (arg.find("+disk=") == 0)
      dynamic_devices.push_back(new disk_t(arg.c_str() + strlen("+disk=")));
    else if (arg.find("+signature=") == 0)
      sig_file = arg.c_str() + strlen("+signature=");
    else if (arg.find("+chroot=") == 0)
      set_chroot(arg.substr(strlen("+chroot=")).c_str());
    else if (arg.find("+target-cwd=") == 0)
      target_init_cwd = arg.substr(strlen("+target-cwd="));
  }

  if (target_init_cwd.empty())
    syscall_proxy->init_target_cwd(nullptr);
  else
    syscall_proxy->init_target_cwd(target_init_cwd.c_str());

  device_list.register_device(syscall_proxy);
  device_list.register_device(&bcd);
  for (auto d : dynamic_devices)
    device_list.register_device(d);
}

htif_t::~htif_t()
{
  for (auto d : dynamic_devices)
    delete d;
}

packet_t htif_t::read_packet(seqno_t expected_seqno)
{
  while (1)
  {
    if (read_buf.size() >= sizeof(packet_header_t))
    {
      packet_header_t hdr(&read_buf[0]);
      if (read_buf.size() >= hdr.get_packet_size())
      {
        packet_t p(&read_buf[0]);
        switch (p.get_header().cmd)
        {
          case HTIF_CMD_ACK:
            break;
          case HTIF_CMD_NACK:
            throw packet_error("nack!");
          default:
            throw packet_error("illegal command " + std::to_string(p.get_header().cmd));
        }
        read_buf.erase(read_buf.begin(), read_buf.begin() + hdr.get_packet_size());
        return p;
      }
    }
    size_t old_size = read_buf.size();
    size_t max_size = sizeof(packet_header_t) + chunk_max_size();
    read_buf.resize(old_size + max_size);
    ssize_t this_size = this->read(&read_buf[old_size], max_size);
    if (this_size < 0)
      throw io_error("read failed");
    read_buf.resize(old_size + this_size);
  }
}

void htif_t::write_packet(const packet_t& p)
{
  for (size_t pos = 0; pos < p.get_size(); )
  {
    ssize_t bytes = this->write(p.get_packet() + pos, p.get_size() - pos);
    if (bytes < 0)
      throw io_error("write failed");
    pos += bytes;
  }
}

void htif_t::start()
{
  assert(!started);
  started = true;

  load_program();
  reset();
}

void htif_t::load_program()
{
  if (targs.size() == 0 || targs[0] == "none")
    return;

  std::string path;
  if (access(targs[0].c_str(), F_OK) == 0)
    path = targs[0];
  else if (targs[0].find('/') == std::string::npos)
  {
    std::string test_path = PREFIX TARGET_DIR + targs[0];
    if (access(test_path.c_str(), F_OK) == 0)
      path = test_path;
  }

  if (path.empty())
    throw std::runtime_error("could not open " + targs[0]);

  std::map<std::string, uint64_t> symbols = load_elf(path.c_str(), &mem);

  // detect torture tests so we can print the memory signature at the end
  if (symbols.count("begin_signature") && symbols.count("end_signature"))
  {
    sig_addr = symbols["begin_signature"];
    sig_len = symbols["end_signature"] - sig_addr;
  }
}

void htif_t::reset()
{
  uint32_t first_words[] = {mem_mb(), num_cores()};
  size_t al = chunk_align();
  uint8_t chunk[(sizeof(first_words)+al-1)/al*al];
  read_chunk(0, sizeof(chunk), chunk);
  memcpy(chunk, first_words, sizeof(first_words));
  write_chunk(0, sizeof(chunk), chunk);

  for (uint32_t i = 0; i < num_cores(); i++)
  {
    write_cr(i, 29, 1);
    write_cr(i, 29, 0);
  }
}

void htif_t::stop()
{
  if (!sig_file.empty() && sig_len) // print final torture test signature
  {
    std::vector<uint8_t> buf(sig_len);
    mem.read(sig_addr, sig_len, &buf[0]);

    std::ofstream sigs(sig_file);
    assert(sigs && "can't open signature file!");
    sigs << std::setfill('0') << std::hex;

    const addr_t incr = 16;
    assert(sig_len % incr == 0);
    for (addr_t i = 0; i < sig_len; i += incr)
    {
      for (addr_t j = incr; j > 0; j--)
        sigs << std::setw(2) << (uint16_t)buf[i+j-1];
      sigs << '\n';
    }

    sigs.close();
  }

  dump_final_state();

  for (uint32_t i = 0, nc = num_cores(); i < nc; i++)
    write_cr(i, 29, 1);

  stopped = true;
}

void htif_t::read_chunk(addr_t taddr, size_t len, void* dst)
{
  assert(taddr % chunk_align() == 0);
  assert(len % chunk_align() == 0 && len <= chunk_max_size());

  packet_header_t hdr(HTIF_CMD_READ_MEM, seqno,
                      len/HTIF_DATA_ALIGN, taddr/HTIF_DATA_ALIGN);
  write_packet(hdr);
  packet_t resp = read_packet(seqno);
  seqno++;

  memcpy(dst, resp.get_payload(), len);
}

void htif_t::write_chunk(addr_t taddr, size_t len, const void* src)
{
  assert(taddr % chunk_align() == 0);
  assert(len % chunk_align() == 0 && len <= chunk_max_size());

  bool nonzero = started || !assume0init();
  for (size_t i = 0; i < len && !nonzero; i++)
    nonzero |= ((uint8_t*)src)[i] != 0;

  if (nonzero)
  {
    packet_header_t hdr(HTIF_CMD_WRITE_MEM, seqno,
                        len/HTIF_DATA_ALIGN, taddr/HTIF_DATA_ALIGN);
    write_packet(packet_t(hdr, src, len));
    read_packet(seqno);
    seqno++;
  }
}

reg_t htif_t::read_cr(uint32_t coreid, uint16_t regnum)
{
  reg_t addr = (reg_t)coreid << 20 | regnum;
  packet_header_t hdr(HTIF_CMD_READ_CONTROL_REG, seqno, 1, addr);
  write_packet(hdr);

  packet_t resp = read_packet(seqno);
  seqno++;

  reg_t val;
  assert(resp.get_payload_size() == sizeof(reg_t));
  memcpy(&val, resp.get_payload(), sizeof(reg_t));
  return val;
}

reg_t htif_t::write_cr(uint32_t coreid, uint16_t regnum, reg_t val)
{
  reg_t addr = (reg_t)coreid << 20 | regnum;
  packet_header_t hdr(HTIF_CMD_WRITE_CONTROL_REG, seqno, 1, addr);
  write_packet(packet_t(hdr, &val, sizeof(val)));

  packet_t resp = read_packet(seqno);
  seqno++;

  assert(resp.get_payload_size() == sizeof(reg_t));
  memcpy(&val, resp.get_payload(), sizeof(reg_t));
  return val;
}

int htif_t::run()
{
  start();
  std::vector<std::queue<reg_t>> fromhost(num_cores());

  auto enq_func = [](std::queue<reg_t>* q, uint64_t x) { q->push(x); };
  std::vector<std::function<void(reg_t)>> fromhost_callbacks;
  for (size_t i = 0; i < num_cores(); i++)
    fromhost_callbacks.push_back(std::bind(enq_func, &fromhost[i], std::placeholders::_1));

  while (!signal_exit && exitcode == 0)
  {
    for (uint32_t coreid = 0; coreid < num_cores(); coreid++)
    {
      if (auto tohost = write_cr(coreid, 30, 0))
      {
        command_t cmd(this, tohost, fromhost_callbacks[coreid], coreid);
        device_list.handle_command(cmd);
      }

      device_list.tick();

      if (!fromhost[coreid].empty())
        if (write_cr(coreid, 31, fromhost[coreid].front()) == 0)
          fromhost[coreid].pop();
    }
  }

  stop();

  return exit_code();
}

uint32_t htif_t::num_cores()
{
  if (_num_cores == 0)
    _num_cores = read_cr(-1, 0);
  return _num_cores;
}

uint32_t htif_t::mem_mb()
{
  if (_mem_mb == 0)
    _mem_mb = read_cr(-1, 1);
  return _mem_mb;
}

bool htif_t::done()
{
  return stopped;
}

int htif_t::exit_code()
{
  return exitcode >> 1;
}

void htif_t::set_state_dump_path(std::string dump_path)
{
  if (this->stats_dump_fd)
    fclose(this->stats_dump_fd);

  this->stats_dump_fd = fopen(dump_path.c_str(), "w");
  if (this->stats_dump_fd == NULL)
  {
    throw std::runtime_error(
        "Fail to create state dump file at \"" + dump_path + "\", reason: " + std::string(std::strerror(errno)));
  }
}

// dump states to this->stats_dump_fd in JSON format
void htif_t::dump_final_state()
{
  if (this->stats_dump_fd == NULL)
    return;

  const size_t n_cpus = num_cores();
  fprintf(this->stats_dump_fd, "{\n");
  fprintf(this->stats_dump_fd, "  \"core_state\": [\n");
  for (size_t n = 0; n < n_cpus; n++)
  {
    fprintf(this->stats_dump_fd, "    {\n");

    // dump instret
    uint64_t instret = read_cr(n, 6);
    fprintf(this->stats_dump_fd, "      \"instret\": %" PRIu64 "\n", instret);

    if (n == n_cpus - 1)
      fprintf(this->stats_dump_fd, "    }\n");
    else
      fprintf(this->stats_dump_fd, "    },\n");
  }
  fprintf(this->stats_dump_fd, "  ]\n");
  fprintf(this->stats_dump_fd, "}\n");
  fclose(this->stats_dump_fd);
  this->stats_dump_fd = NULL;
}
