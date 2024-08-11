from pathlib import Path


def trumptcp_find_interrupt(self):

    self.mkcom_with_ia16("tcpfintr", r"""\
#include <dos.h>
#include <i86.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

char *addr2str(uint32_t val)
{
   static char buf[32];

   snprintf(buf, sizeof(buf), "%lu.%lu.%lu.%lu",
       (val & 0x000000ff),
       (val & 0x0000ff00) >>  8,
       (val & 0x00ff0000) >> 16,
       (val & 0xff000000) >> 24);
   return buf;
}

/*
  driver_info  determine if driver is valid and the functionality of the driver

    call
      ah   = 00H
      al   = FFH

    return
      al   = FFH     no driver loaded

      al   = 0  driver loaded
      dh   = additional functionality flags
        1    = implements timeouts
        2    = implements async i/o via events
      ds:si = pointer to "TCP_DRVR"
      es:di     = pointer to driver info
                    (including IP address, net params, etc)
*/

int driver_info(int inum)
{
  struct driver_info {
    uint32_t ip;
    uint32_t netmask;
    uint32_t gateway;
    uint32_t dnsserver;
    uint32_t timeserver;
    uint16_t mtu;
    uint8_t  def_ttl;
    uint8_t  def_tos;
    uint16_t tcp_mss;
    uint16_t tcp_rwin;
    uint16_t debug;
    uint8_t domlen; char domain[255]; /* Pascal string */

    /*
      from https://web.archive.org/web/20100127193745/http://alumnus.caltech.edu/~dank/trumpet/tabi32.zip
      they seem to be in the v3.x stack.
     */

#define DNSLISTLEN (4)
#define TIMELISTLEN (4)
    uint32_t dns_list[DNSLISTLEN];
    uint32_t time_list[TIMELISTLEN];
    uint32_t ip_dropped;
    uint32_t tcp_dropped;
    uint32_t udp_dropped;
  } __attribute__((packed));

  union REGS r = {};
  struct SREGS rs;
  char __far * dstring;
  char buf[9];
  char buf2[256];
  struct driver_info __far *di;

  segread(&rs);

  r.h.ah = 0x00;
  r.h.al = 0xff;

  int86x(inum, &r, &r, &rs);

  if (r.h.al == 0xff) {
    printf("INFO: driver not present (AX == 0x%04x)\n", r.w.ax);
    return 0;
  }

  printf("INFO: timeouts %senabled\n", (r.h.dh & 0x1) ? "" : "not ");
  printf("INFO: async events %senabled\n", (r.h.dh & 0x2) ? "" : "not ");

  dstring = MK_FP(rs.ds, r.x.si);
  _fmemcpy(&buf, dstring, 8);
  buf[8] = '\0';
  printf("INFO: driver string = '%s'\n", buf);

  di = MK_FP(rs.es, r.x.di);
  printf("INFO: ip is '%s'\n", addr2str(di->ip));
  printf("INFO: netmask is '%s'\n", addr2str(di->netmask));
  printf("INFO: gateway is '%s'\n", addr2str(di->gateway));
  printf("INFO: dnsserver is '%s'\n", addr2str(di->dnsserver));
  printf("INFO: timeserver is '%s'\n", addr2str(di->timeserver));
  printf("INFO: mtu is '%u'\n", di->mtu);
  printf("INFO: def_ttl is '%u'\n", di->def_ttl);
  printf("INFO: def_tos is '%u'\n", di->def_tos);
  printf("INFO: tcp_mss is '%u'\n", di->tcp_mss);
  printf("INFO: tcp_rwin is '%u'\n", di->tcp_rwin);

  printf("INFO: domain len is '%u'\n", di->domlen);
  if (di->domlen < sizeof(buf2)) {
    _fmemcpy(buf2, di->domain, di->domlen);
    buf2[di->domlen] = '\0';
    printf("INFO: domain is '%s'\n", buf2);
  }

  return 1;
}

int find_driver(void)
{
  char __far *ivec;
  int i;

  for (i = 0x60; i <= 0xff; i++) {
    ivec = (char __far *)_dos_getvect(i);

    if (_fmemcmp(ivec + 3, "TCP_DRVR", 8) == 0) {
      printf("INFO: ivec(0x%02x) = %04x:%04x\n", i, FP_SEG(ivec), FP_OFF(ivec));
      return i;
    }
  }

  return 0;
}

int main(void)
{
  int ret = 0, inum;

  inum = find_driver();
  if (inum) {
    if (driver_info(inum)) {
    } else {
      printf("FAIL: Couldn't get driver info\n");
      ret = -1;
    }
  } else {
    printf("FAIL: Driver not found\n");
    ret = -1;
  }

  if (ret == 0)
    printf("PASS:\n");
  return ret;
}
""")

    self.mkfile("testit.bat", """\
c:\\tcpfintr
rem end
""", newline="\r\n")


    results = self.runDosemu("testit.bat", config="""\
$_hdimage = "dXXXXs/c:hdtype1 +1"
$_floppy_a = ""
$_tcpdriver = (on)
""")

    self.assertNotIn("FAIL", results)
    self.assertIn("INFO: domain is 'localdomain'", results)
