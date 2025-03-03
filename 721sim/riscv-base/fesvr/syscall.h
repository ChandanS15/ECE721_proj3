// See LICENSE for license details.

#ifndef __SYSCALL_H
#define __SYSCALL_H

#include "device.h"
#include <vector>
#include <string>
#include <map>

class syscall_t;
typedef reg_t (syscall_t::*syscall_func_t)(reg_t, reg_t, reg_t, reg_t, reg_t, reg_t, reg_t);

class htif_t;
class memif_t;
class strace;
class target_cwd;

class fds_t
{
 public:
  target_cwd* cwd_info;
  reg_t alloc(int fd);
  void dealloc(reg_t fd);
  int lookup(reg_t fd);
 private:
  std::vector<int> fds;
};

class syscall_t : public device_t
{
 public:
  syscall_t(htif_t*);
  ~syscall_t();
  virtual void enable_strace(const char* output_path);
  virtual void dump_std_out_err(const char* stdout_dump_path, const char* stderr_dump_path);
  virtual void init_target_cwd(const char* cwd);

 private:
  strace* m_strace;
  target_cwd* m_target_cwd;
  int stdout_dump_fd = -1;
  int stderr_dump_fd = -1;
  const char* identity() { return "syscall_proxy"; }

  htif_t* htif;
  memif_t* memif;
  std::vector<syscall_func_t> table;
  fds_t fds;

  uint16_t sys_getrandom_rand_xsubi[3];

  void handle_syscall(command_t cmd);
  void dispatch(addr_t mm);
  std::string do_chroot(const char* fn);
  std::string undo_chroot(const char* fn);

  reg_t sys_exit(reg_t, reg_t, reg_t, reg_t, reg_t, reg_t, reg_t);
  reg_t sys_openat(reg_t, reg_t, reg_t, reg_t, reg_t, reg_t, reg_t);
  reg_t sys_read(reg_t, reg_t, reg_t, reg_t, reg_t, reg_t, reg_t);
  reg_t sys_pread(reg_t, reg_t, reg_t, reg_t, reg_t, reg_t, reg_t);
  reg_t sys_write(reg_t, reg_t, reg_t, reg_t, reg_t, reg_t, reg_t);
  reg_t sys_pwrite(reg_t, reg_t, reg_t, reg_t, reg_t, reg_t, reg_t);
  reg_t sys_close(reg_t, reg_t, reg_t, reg_t, reg_t, reg_t, reg_t);
  reg_t sys_lseek(reg_t, reg_t, reg_t, reg_t, reg_t, reg_t, reg_t);
  reg_t sys_fstat(reg_t, reg_t, reg_t, reg_t, reg_t, reg_t, reg_t);
  reg_t sys_lstat(reg_t, reg_t, reg_t, reg_t, reg_t, reg_t, reg_t);
  reg_t sys_fstatat(reg_t, reg_t, reg_t, reg_t, reg_t, reg_t, reg_t);
  reg_t sys_faccessat(reg_t, reg_t, reg_t, reg_t, reg_t, reg_t, reg_t);
  reg_t sys_fcntl(reg_t, reg_t, reg_t, reg_t, reg_t, reg_t, reg_t);
  reg_t sys_linkat(reg_t, reg_t, reg_t, reg_t, reg_t, reg_t, reg_t);
  reg_t sys_unlinkat(reg_t, reg_t, reg_t, reg_t, reg_t, reg_t, reg_t);
  reg_t sys_mkdirat(reg_t, reg_t, reg_t, reg_t, reg_t, reg_t, reg_t);
  reg_t sys_getcwd(reg_t, reg_t, reg_t, reg_t, reg_t, reg_t, reg_t);
  reg_t sys_readlinkat(reg_t, reg_t, reg_t, reg_t, reg_t, reg_t, reg_t);
  reg_t sys_getmainvars(reg_t, reg_t, reg_t, reg_t, reg_t, reg_t, reg_t);
  reg_t sys_ftruncate(reg_t, reg_t, reg_t, reg_t, reg_t, reg_t, reg_t);
  reg_t sys_chdir(reg_t, reg_t, reg_t, reg_t, reg_t, reg_t, reg_t);
  reg_t sys_getdents64(reg_t, reg_t, reg_t, reg_t, reg_t, reg_t, reg_t);
  reg_t sys_getrandom(reg_t, reg_t, reg_t, reg_t, reg_t, reg_t, reg_t);
  reg_t sys_renameat2(reg_t, reg_t, reg_t, reg_t, reg_t, reg_t, reg_t);

  friend target_cwd;
  friend class syscall_main_t;
  friend class syscall_mirror_t;
};

#endif
