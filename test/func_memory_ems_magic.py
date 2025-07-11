"""
    Reimplemention of the test suite from EMS Magic. The original was in
    some dialect of QBasic 4.5 and released here:
    https://forum.vcfed.org/index.php?threads/ems-driver-test-suite.1253490/
    I contacted the author regarding its licence:
    ~~~
    >> Hi Plasma, thanks for sharing your comprehensive test suite. I'm
    just starting to use it to validate the EMS functionality in Dosemu2,
    and already we found an issue. In the future I might like to add some or
    all of it to our own CI tests can I ask what licence you are using for it?
    For our purposes it would be nice if it were GPLv2+ or MIT and then it
    could be added to the repository. I see you are using a basic compiler for
    it, what flavour would that be?

    Many Thanks.

    > Thanks for asking. I'm fine with making the code public domain. It was
    only intended for my own use, so it will need changes to be a
    general-purpose test. But you are welcome to do whatever you want with it.
    Most of the code is QuickBASIC 4.5 with QBMCX. The assembly is MASM 5 or 6.
    ~~~
"""

import re

BATCHFILE = """\
c:\\%s
rem end
"""

def test_memory_ems_magic_00(self):
    """Memory EMS (EMS Magic) [00] EMM Installed"""

    self.mkfile("testit.bat", BATCHFILE % 'ems_00', newline="\r\n")

    self.mkcom_with_ia16("ems_00", r"""

#include <i86.h>
#include <stdio.h>
#include <string.h>

static char magic[] = "EMMXXXX0";


int main(int argc, char *argv[])
{
  union REGS r = {};
  struct SREGS rs;
  char __far *sig;
  char __far *mag;

  r.x.ax = 0x3567;      // get interrupt vector
  int86x(0x21, &r, &r, &rs);

  sig = MK_FP(rs.es, 10);
  mag = MK_FP(FP_SEG(magic), FP_OFF(magic));

  if (_fmemcmp(sig, mag, 8) == 0) {
    printf("PASS: EMM driver found\n");
  } else {
    printf("INFO: EMM driver not found\n");
  }

  return 0;
}

""")

    results = self.runDosemu("testit.bat")

    if 'INFO: EMM driver not found' in results:
        self.__class__.noems = True
        self.skipTest("no EMM installed")

    self.assertIn("PASS:", results)


def test_memory_ems_magic_01(self):
    """Memory EMS (EMS Magic) [01] Get Status"""

    if getattr(self, 'noems', False):
        self.skipTest("no EMM installed")

    self.mkfile("testit.bat", BATCHFILE % 'ems_01', newline="\r\n")

    self.mkcom_with_ia16("ems_01", r"""

#include <i86.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char *argv[])
{
  union REGS r = {};
  struct SREGS rs;

  r.x.ax = 0x4000;      // get status
  int86x(0x67, &r, &r, &rs);

  printf("INFO: Status = 0x%02x\n", r.h.ah);

  return 0;
}

""")

    results = self.runDosemu("testit.bat")
    self.assertIn("INFO: Status = 0x00", results)


def test_memory_ems_magic_02(self):
    """Memory EMS (EMS Magic) [02] Get Page Frame Address"""

    if getattr(self, 'noems', False):
        self.skipTest("no EMM installed")

    self.mkfile("testit.bat", BATCHFILE % 'ems_02', newline="\r\n")

    self.mkcom_with_ia16("ems_02", r"""

#include <i86.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char *argv[])
{
  union REGS r = {};
  struct SREGS rs;

  r.x.ax = 0x4100;      // get page frame address
  int86x(0x67, &r, &r, &rs);

  printf("INFO: Status = 0x%02x\n", r.h.ah);  // should be 0 if pageframe exists, otherwise 8000h
  printf("INFO: Address = 0x%04x\n", r.x.bx); // pageframe address

  return 0;
}

""")

    results = self.runDosemu("testit.bat")
    self.assertIn("INFO: Status = 0x00", results)
    self.assertIn("INFO: Address = 0xe000", results)


def test_memory_ems_magic_03(self):
    """Memory EMS (EMS Magic) [03] Get Unallocated Page Count"""

    if getattr(self, 'noems', False):
        self.skipTest("no EMM installed")

    self.mkfile("testit.bat", BATCHFILE % 'ems_03', newline="\r\n")

    self.mkcom_with_ia16("ems_03", r"""

#include <i86.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char *argv[])
{
  union REGS r = {};
  struct SREGS rs;

  r.x.ax = 0x4200;      // Get Unallocated Page Count
  int86x(0x67, &r, &r, &rs);

  printf("INFO: Status = 0x%02x\n", r.h.ah);  // should be 0
  printf("INFO: Unallocated Pages = 0x%04x\n", r.x.bx);
  printf("INFO: Total Pages = 0x%04x\n", r.x.dx);

  return 0;
}

""")

    results = self.runDosemu("testit.bat")
    self.assertIn("INFO: Status = 0x00", results)

    r1 = re.compile(r'INFO: Unallocated Pages = 0x([0-9a-f]+)')
    self.assertRegex(results, r1)
    t = r1.search(results)
    unallocated_pages = int(t.group(1), 16)

    r2 = re.compile(r'INFO: Total Pages = 0x([0-9a-f]+)')
    self.assertRegex(results, r2)
    t = r2.search(results)
    total_pages = int(t.group(1), 16)

    self.assertGreaterEqual(unallocated_pages, 0x100, results)
    self.assertLessEqual(unallocated_pages, 0x280, results)
    self.assertGreaterEqual(total_pages, 0x100, results)
    self.assertLessEqual(total_pages, 0x280, results)

    self.assertGreaterEqual(total_pages, unallocated_pages, results)


def test_memory_ems_magic_04(self):
    """Memory EMS (EMS Magic) [04] Allocate Zero Pages (fn 4)"""

    if getattr(self, 'noems', False):
        self.skipTest("no EMM installed")

    self.mkfile("testit.bat", BATCHFILE % 'ems_04', newline="\r\n")

    self.mkcom_with_ia16("ems_04", r"""

#include <i86.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char *argv[])
{
  union REGS r = {};
  struct SREGS rs;

  r.x.ax = 0x4300;      // Allocate Zero Pages (function 4)
  r.x.bx = 0;
  int86x(0x67, &r, &r, &rs);

  printf("INFO: Status = 0x%02x\n", r.h.ah);  // should be 0x89 ( attempt to allocate zero pages)
  printf("INFO: Handle = 0x%04x\n", r.x.dx);

  return 0;
}

""")

    results = self.runDosemu("testit.bat")
    self.assertIn("INFO: Status = 0x89", results)


def test_memory_ems_magic_05(self):
    """Memory EMS (EMS Magic) [05] Allocate 100 Pages (fn 4)"""

    if getattr(self, 'noems', False):
        self.skipTest("no EMM installed")

    self.mkfile("testit.bat", BATCHFILE % 'ems_05', newline="\r\n")

    self.mkcom_with_ia16("ems_05", r"""

#include <i86.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char *argv[])
{
  union REGS r = {};
  struct SREGS rs;

  r.x.ax = 0x4300;      // Allocate 100 Pages (function 4)
  r.x.bx = 100;
  int86x(0x67, &r, &r, &rs);

  printf("INFO: Status = 0x%02x\n", r.h.ah);  // should be 0
  printf("INFO: Handle = 0x%04x\n", r.x.dx);

  return 0;
}

""")

    results = self.runDosemu("testit.bat")
    self.assertIn("INFO: Status = 0x00", results)
    r2 = re.compile(r'INFO: Handle = 0x([0-9a-f]+)')
    self.assertRegex(results, r2)
    t = r2.search(results)
    handle = int(t.group(1), 16)

    self.assertGreaterEqual(handle, 0x1, results)


def test_memory_ems_magic_06(self):
    """Memory EMS (EMS Magic) [06] Allocate Zero Std Pages (fn 27)"""

    if getattr(self, 'noems', False):
        self.skipTest("no EMM installed")

    self.mkfile("testit.bat", BATCHFILE % 'ems_06', newline="\r\n")

    self.mkcom_with_ia16("ems_06", r"""

#include <i86.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char *argv[])
{
  union REGS r = {};
  struct SREGS rs;

  r.x.ax = 0x5a00;      // Allocate Zero Standard Pages (function 27)
  r.x.bx = 0;
  int86x(0x67, &r, &r, &rs);

  printf("INFO: Status = 0x%02x\n", r.h.ah);  // should be 0
  printf("INFO: Handle = 0x%04x\n", r.x.dx);

  return 0;
}

""")

    results = self.runDosemu("testit.bat")
    self.assertIn("INFO: Status = 0x00", results)
    r2 = re.compile(r'INFO: Handle = 0x([0-9a-f]+)')
    self.assertRegex(results, r2)
    t = r2.search(results)
    handle = int(t.group(1), 16)

    self.assertGreaterEqual(handle, 0x1, results)


def test_memory_ems_magic_07(self):
    """Memory EMS (EMS Magic) [07] Allocate 100 Raw Pages (fn 27)"""

    if getattr(self, 'noems', False):
        self.skipTest("no EMM installed")

    self.mkfile("testit.bat", BATCHFILE % 'ems_07', newline="\r\n")

    self.mkcom_with_ia16("ems_07", r"""

#include <i86.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char *argv[])
{
  union REGS r = {};
  struct SREGS rs;

  r.x.ax = 0x5a01;      // Allocate 100 Raw Pages (function 27)
  r.x.bx = 100;
  int86x(0x67, &r, &r, &rs);

  printf("INFO: Status = 0x%02x\n", r.h.ah);  // should be 0
  printf("INFO: Handle = 0x%04x\n", r.x.dx);

  return 0;
}

""")

    results = self.runDosemu("testit.bat")
    self.assertIn("INFO: Status = 0x00", results)
    r2 = re.compile(r'INFO: Handle = 0x([0-9a-f]+)')
    self.assertRegex(results, r2)
    t = r2.search(results)
    handle = int(t.group(1), 16)

    self.assertGreaterEqual(handle, 0x1, results)


def test_memory_ems_magic_08(self):
    """Memory EMS (EMS Magic) [08] Reallocate To Zero Pages"""

    if getattr(self, 'noems', False):
        self.skipTest("no EMM installed")

    self.mkfile("testit.bat", BATCHFILE % 'ems_08', newline="\r\n")

    self.mkcom_with_ia16("ems_08", r"""

#include <i86.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char *argv[])
{
  union REGS r = {};
  struct SREGS rs;
  uint16_t handle;

  r.x.ax = 0x4300;      // Allocate 100 Pages
  r.x.bx = 100;
  int86x(0x67, &r, &r, &rs);
  if (r.h.ah != 0) {
    printf("FAIL: Status1 = 0x%02x\n", r.h.ah); // should be 0
    return 1;
  }
  handle = r.x.dx;
  printf("INFO: Handle = 0x%04x\n", handle);

  r.x.ax = 0x5100;      // Reallocate to zero pages
  r.x.bx = 0;
  r.x.dx = handle;
  int86x(0x67, &r, &r, &rs);
  if (r.h.ah != 0) {
    printf("FAIL: Status2 = 0x%02x\n", r.h.ah); // should be 0
    return 1;
  }
  printf("INFO: Num Pages Allocated = 0x%04x\n", r.x.bx);

  return 0;
}

""")

    results = self.runDosemu("testit.bat")

    r2 = re.compile(r'INFO: Handle = 0x([0-9a-f]+)')
    self.assertRegex(results, r2)
    t = r2.search(results)
    handle = int(t.group(1), 16)
    self.assertGreaterEqual(handle, 0x1, results)

    self.assertIn("INFO: Num Pages Allocated = 0x0000", results)

    self.assertNotIn("FAIL:", results)


def test_memory_ems_magic_09(self):
    """Memory EMS (EMS Magic) [09] Reallocate To 250 Pages"""

    if getattr(self, 'noems', False):
        self.skipTest("no EMM installed")

    self.mkfile("testit.bat", BATCHFILE % 'ems_09', newline="\r\n")

    self.mkcom_with_ia16("ems_09", r"""

#include <i86.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char *argv[])
{
  union REGS r = {};
  struct SREGS rs;
  uint16_t handle;

  r.x.ax = 0x4300;      // Allocate 100 Pages
  r.x.bx = 100;
  int86x(0x67, &r, &r, &rs);
  if (r.h.ah != 0) {
    printf("FAIL: Status1 = 0x%02x\n", r.h.ah); // should be 0
    return 1;
  }
  handle = r.x.dx;
  printf("INFO: Handle = 0x%04x\n", handle);

  r.x.ax = 0x5100;      // Reallocate to 250 pages
  r.x.bx = 250;
  r.x.dx = handle;
  int86x(0x67, &r, &r, &rs);
  if (r.h.ah != 0) {
    printf("FAIL: Status2 = 0x%02x\n", r.h.ah); // should be 0
    return 1;
  }
  printf("INFO: Num Pages Allocated = 0x%04x\n", r.x.bx);

  return 0;
}

""")

    results = self.runDosemu("testit.bat")

    r2 = re.compile(r'INFO: Handle = 0x([0-9a-f]+)')
    self.assertRegex(results, r2)
    t = r2.search(results)
    handle = int(t.group(1), 16)
    self.assertGreaterEqual(handle, 0x1, results)

    self.assertIn("INFO: Num Pages Allocated = 0x00fa", results)

    self.assertNotIn("FAIL:", results)


def test_memory_ems_magic_10(self):
    """Memory EMS (EMS Magic) [10] Reallocate To 1200 (> System) Pages"""

    if getattr(self, 'noems', False):
        self.skipTest("no EMM installed")

    self.mkfile("testit.bat", BATCHFILE % 'ems_10', newline="\r\n")

    self.mkcom_with_ia16("ems_10", r"""

#include <i86.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char *argv[])
{
  union REGS r = {};
  struct SREGS rs;
  uint16_t handle;

  r.x.ax = 0x4300;      // Allocate 100 Pages
  r.x.bx = 100;
  int86x(0x67, &r, &r, &rs);
  if (r.h.ah != 0) {
    printf("FAIL: Status1 = 0x%02x\n", r.h.ah); // should be 0
    return 1;
  }
  handle = r.x.dx;
  printf("INFO: Handle = 0x%04x\n", handle);

  r.x.ax = 0x5100;      // Reallocate to 1200 pages
  r.x.bx = 1200;
  r.x.dx = handle;
  int86x(0x67, &r, &r, &rs);
  if (r.h.ah == 0) {
    printf("FAIL: Status2 = SUCCESS\n");
    return 1;
  }
  printf("INFO: Error = 0x%02x\n", r.h.ah); // should be 87h
  printf("INFO: Num Pages Allocated = 0x%04x\n", r.x.bx); // 100(0x64)

  return 0;
}

""")

    results = self.runDosemu("testit.bat")

    r2 = re.compile(r'INFO: Handle = 0x([0-9a-f]+)')
    self.assertRegex(results, r2)
    t = r2.search(results)
    handle = int(t.group(1), 16)
    self.assertGreaterEqual(handle, 0x1, results)

    self.assertIn("INFO: Error = 0x87", results)

    self.assertIn("INFO: Num Pages Allocated = 0x0064", results)

    self.assertNotIn("FAIL:", results)


def test_memory_ems_magic_11(self):
    """Memory EMS (EMS Magic) [11] Reallocate To 510 (> Avail) Pages"""

    if getattr(self, 'noems', False):
        self.skipTest("no EMM installed")

    self.mkfile("testit.bat", BATCHFILE % 'ems_11', newline="\r\n")

    self.mkcom_with_ia16("ems_11", r"""

#include <i86.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char *argv[])
{
  union REGS r = {};
  struct SREGS rs;
  uint16_t handle;

  r.x.ax = 0x4300;      // Allocate 100 Pages
  r.x.bx = 100;
  int86x(0x67, &r, &r, &rs);
  if (r.h.ah != 0) {
    printf("FAIL: Status1 = 0x%02x\n", r.h.ah); // should be 0
    return 1;
  }
  handle = r.x.dx;
  printf("INFO: Handle = 0x%04x\n", handle);

  r.x.ax = 0x5100;      // Reallocate to 510 pages (System = 535, Used = 24/28)
  r.x.bx = 510;
  r.x.dx = handle;
  int86x(0x67, &r, &r, &rs);
  if (r.h.ah == 0) {
    printf("FAIL: Status2 = SUCCESS\n");
    return 1;
  }
  printf("INFO: Error = 0x%02x\n", r.h.ah); // should be 88h
  printf("INFO: Num Pages Allocated = 0x%04x\n", r.x.bx); // 100(0x64)

  return 0;
}

""")

    results = self.runDosemu("testit.bat")

    r2 = re.compile(r'INFO: Handle = 0x([0-9a-f]+)')
    self.assertRegex(results, r2)
    t = r2.search(results)
    handle = int(t.group(1), 16)
    self.assertGreaterEqual(handle, 0x1, results)

    self.assertIn("INFO: Error = 0x88", results)

    self.assertIn("INFO: Num Pages Allocated = 0x0064", results)

    self.assertNotIn("FAIL:", results)


def test_memory_ems_magic_12(self):
    """Memory EMS (EMS Magic) [12] Reallocate To Invalid Handle"""

    # Test 12 would be a duplicate of test 09 but with 450 pages instead of
    # 250, so let's test for an invalid handle instead

    if getattr(self, 'noems', False):
        self.skipTest("no EMM installed")

    self.mkfile("testit.bat", BATCHFILE % 'ems_12', newline="\r\n")

    self.mkcom_with_ia16("ems_12", r"""

#include <i86.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char *argv[])
{
  union REGS r = {};
  struct SREGS rs;
  uint16_t handle;

  r.x.ax = 0x4300;      // Allocate 100 Pages
  r.x.bx = 100;
  int86x(0x67, &r, &r, &rs);
  if (r.h.ah != 0) {
    printf("FAIL: Status1 = 0x%02x\n", r.h.ah); // should be 0
    return 1;
  }
  handle = r.x.dx;
  printf("INFO: Handle = 0x%04x\n", handle);

  r.x.ax = 0x5100;      // Reallocate to 450 pages but to an invalid handle
  r.x.bx = 450;
  r.x.dx = handle + 5;  // We assume this is invalid
  int86x(0x67, &r, &r, &rs);
  if (r.h.ah == 0) {
    printf("FAIL: Status2 = SUCCESS\n");
    return 1;
  }
  printf("INFO: Error = 0x%02x\n", r.h.ah); // should be 83h

  return 0;
}

""")

    results = self.runDosemu("testit.bat")

    r2 = re.compile(r'INFO: Handle = 0x([0-9a-f]+)')
    self.assertRegex(results, r2)
    t = r2.search(results)
    handle = int(t.group(1), 16)
    self.assertGreaterEqual(handle, 0x1, results)

    self.assertIn("INFO: Error = 0x83", results)

    self.assertNotIn("FAIL:", results)


def test_memory_ems_magic_13(self):
    """Memory EMS (EMS Magic) [13] Get Pages With Invalid Handle"""

    if getattr(self, 'noems', False):
        self.skipTest("no EMM installed")

    self.mkfile("testit.bat", BATCHFILE % 'ems_13', newline="\r\n")

    self.mkcom_with_ia16("ems_13", r"""

#include <i86.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char *argv[])
{
  union REGS r = {};
  struct SREGS rs;
  uint16_t handle;

  r.x.ax = 0x4300;      // Allocate 100 Pages
  r.x.bx = 100;
  int86x(0x67, &r, &r, &rs);
  if (r.h.ah != 0) {
    printf("FAIL: Status1 = 0x%02x\n", r.h.ah); // should be 0
    return 1;
  }
  handle = r.x.dx;
  printf("INFO: Handle = 0x%04x\n", handle);

  r.x.ax = 0x4c00;      // Get number of pages allocated to handle
  r.x.dx = handle + 5;  // We assume this is invalid
  int86x(0x67, &r, &r, &rs);
  if (r.h.ah == 0) {
    printf("FAIL: Status2 = 0x%02x\n", r.h.ah); // should be non zero
    return 1;
  }
  printf("INFO: Error = 0x%02x\n", r.h.ah); // should be 83h

  return 0;
}

""")

    results = self.runDosemu("testit.bat")

    r2 = re.compile(r'INFO: Handle = 0x([0-9a-f]+)')
    self.assertRegex(results, r2)
    t = r2.search(results)
    handle = int(t.group(1), 16)
    self.assertGreaterEqual(handle, 0x1, results)

    self.assertIn("INFO: Error = 0x83", results)

    self.assertNotIn("FAIL:", results)


def test_memory_ems_magic_14(self):
    """Memory EMS (EMS Magic) [14] Get Pages Of SYSTEM Handle"""

    if getattr(self, 'noems', False):
        self.skipTest("no EMM installed")

    self.mkfile("testit.bat", BATCHFILE % 'ems_14', newline="\r\n")

    self.mkcom_with_ia16("ems_14", r"""

#include <i86.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char *argv[])
{
  union REGS r = {};
  struct SREGS rs;

  r.x.ax = 0x4c00;      // Get number of pages allocated to handle
  r.x.dx = 0;
  int86x(0x67, &r, &r, &rs);
  if (r.h.ah != 0) {
    printf("FAIL: Status = 0x%02x\n", r.h.ah);
    return 1;
  }
  printf("INFO: Handle 0 (SYSTEM) has 0x%04x Pages\n", r.x.bx);

  return 0;
}

""")

    results = self.runDosemu("testit.bat")

    self.assertIn("INFO: Handle 0 (SYSTEM) has 0x0018 Pages", results)

    self.assertNotIn("FAIL:", results)


def test_memory_ems_magic_15(self):
    """Memory EMS (EMS Magic) [15] Get Pages Of Our Handle"""

    if getattr(self, 'noems', False):
        self.skipTest("no EMM installed")

    self.mkfile("testit.bat", BATCHFILE % 'ems_15', newline="\r\n")

    self.mkcom_with_ia16("ems_15", r"""

#include <i86.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char *argv[])
{
  union REGS r = {};
  struct SREGS rs;
  uint16_t handle;

  r.x.ax = 0x4300;      // Allocate 100 Pages
  r.x.bx = 100;
  int86x(0x67, &r, &r, &rs);
  if (r.h.ah != 0) {
    printf("FAIL: Status1 = 0x%02x\n", r.h.ah);
    return 1;
  }
  handle = r.x.dx;
  printf("INFO: Handle = 0x%04x\n", handle);

  r.x.ax = 0x4c00;      // Get number of pages allocated to handle
  r.x.dx = handle;
  int86x(0x67, &r, &r, &rs);
  if (r.h.ah != 0) {
    printf("FAIL: Status = 0x%02x\n", r.h.ah);
    return 1;
  }
  printf("INFO: Handle %d has 0x%04x Pages\n", handle, r.x.bx);

  return 0;
}

""")

    results = self.runDosemu("testit.bat")

    r2 = re.compile(r'INFO: Handle = 0x([0-9a-f]+)')
    self.assertRegex(results, r2)
    t = r2.search(results)
    handle = int(t.group(1), 16)
    self.assertGreaterEqual(handle, 0x1, results)

    self.assertIn("INFO: Handle %d has 0x0064 Pages" % handle, results)

    self.assertNotIn("FAIL:", results)


def test_memory_ems_magic_16(self):
    """Memory EMS (EMS Magic) [16] Get Number Of Pages For Each Handle"""

    if getattr(self, 'noems', False):
        self.skipTest("no EMM installed")

    self.mkfile("testit.bat", BATCHFILE % 'ems_16', newline="\r\n")

    self.mkcom_with_ia16("ems_16", r"""

#include <i86.h>
#include <stdio.h>
#include <string.h>

struct entry {
  uint16_t handle;
  uint16_t pages;
};

struct entry entries[256];


int main(int argc, char *argv[])
{
  union REGS r = {};
  struct SREGS rs;
  uint16_t handle1, handle2;
  int i;

  r.x.ax = 0x4300;      // Allocate 100 Pages
  r.x.bx = 100;
  int86x(0x67, &r, &r, &rs);
  if (r.h.ah != 0) {
    printf("FAIL: Status1 = 0x%02x\n", r.h.ah);
    return 1;
  }
  handle1 = r.x.dx;
  printf("INFO: Handle1 = 0x%04x\n", handle1);

  r.x.ax = 0x4300;      // Allocate 250 Pages
  r.x.bx = 250;
  int86x(0x67, &r, &r, &rs);
  if (r.h.ah != 0) {
    printf("FAIL: Status2 = 0x%02x\n", r.h.ah);
    return 1;
  }
  handle2 = r.x.dx;
  printf("INFO: Handle2 = 0x%04x\n", handle2);

  r.x.ax = 0x4d00;      // Get All Handles Pages
  rs.es = FP_SEG(entries);
  r.x.di = FP_OFF(entries);
  int86x(0x67, &r, &r, &rs);

  if (r.h.ah != 0) {
    printf("FAIL: Status3 = 0x%02x\n", r.h.ah);
    return 1;
  }

  printf("INFO: Handles = 0x%02x\n", r.x.bx);

  for (i=0; i<r.x.bx;i++) {
    printf("INFO:   Handle[%02d], Pages = 0x%02x\n",
        entries[i].handle, entries[i].pages);
  }
  return 0;
}

""")

    results = self.runDosemu("testit.bat")
    self.assertNotIn("FAIL:", results)
    self.assertIn("INFO:   Handle[00], Pages = 0x18", results)

    r2 = re.compile(r'INFO: Handles = 0x([0-9a-f]+)')
    self.assertRegex(results, r2)
    t = r2.search(results)
    handles = int(t.group(1), 16)
    self.assertGreaterEqual(handles, 0x3, results)
    self.assertLessEqual(handles, 0x4, results)

    if handles == 4:
        self.assertIn("INFO:   Handle[01]", results)

    self.assertIn("INFO:   Handle[%02d], Pages = 0x64" % (handles - 2), results)
    self.assertIn("INFO:   Handle[%02d], Pages = 0xfa" % (handles - 1), results)


def test_memory_ems_magic_17(self):
    """Memory EMS (EMS Magic) [17] Invalid Attribute Subfunction"""

    if getattr(self, 'noems', False):
        self.skipTest("no EMM installed")

    self.mkfile("testit.bat", BATCHFILE % 'ems_17', newline="\r\n")

    self.mkcom_with_ia16("ems_17", r"""

#include <i86.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char *argv[])
{
  union REGS r = {};
  struct SREGS rs;

  r.x.ax = 0x5203;      // invalid attribute subfunction
  int86x(0x67, &r, &r, &rs);
  if (r.h.ah != 0x8f) {
    printf("FAIL: Status1 = 0x%02x\n", r.h.ah);
    return 1;
  }

  printf("INFO: Returned invalid subfunction\n");

  return 0;
}

""")

    results = self.runDosemu("testit.bat")

    self.assertIn("INFO: Returned invalid subfunction", results)

    self.assertNotIn("FAIL:", results)


def test_memory_ems_magic_18(self):
    """Memory EMS (EMS Magic) [18] Get Attribute Of SYSTEM Handle"""

    if getattr(self, 'noems', False):
        self.skipTest("no EMM installed")

    self.mkfile("testit.bat", BATCHFILE % 'ems_18', newline="\r\n")

    self.mkcom_with_ia16("ems_18", r"""

#include <i86.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char *argv[])
{
  union REGS r = {};
  struct SREGS rs;

  r.x.ax = 0x5200;      // get attribute
  r.x.dx = 0;
  int86x(0x67, &r, &r, &rs);
  if (r.h.ah != 0x00) {
    printf("FAIL: Status1 = 0x%02x\n", r.h.ah);
    return 1;
  }

  printf("INFO: Returned attribute (0x%02x)\n", r.h.al);

  return 0;
}

""")

    results = self.runDosemu("testit.bat")

    self.assertIn("INFO: Returned attribute (0x00)", results)

    self.assertNotIn("FAIL:", results)


def test_memory_ems_magic_19(self):
    """Memory EMS (EMS Magic) [19] Set Attribute Of SYSTEM Handle"""

    if getattr(self, 'noems', False):
        self.skipTest("no EMM installed")

    self.mkfile("testit.bat", BATCHFILE % 'ems_19', newline="\r\n")

    self.mkcom_with_ia16("ems_19", r"""

#include <i86.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char *argv[])
{
  union REGS r = {};
  struct SREGS rs;

  r.x.ax = 0x5201;      // set attribute
  r.h.bl = 0;           // volatile
  r.x.dx = 0;
  int86x(0x67, &r, &r, &rs);
  if (r.h.ah != 0x00) {
    printf("FAIL: Status1 = 0x%02x\n", r.h.ah);
    return 1;
  }
  printf("INFO: Status1 SUCCESS\n");

  r.x.ax = 0x5201;      // set attribute
  r.h.bl = 1;           // non-volatile
  r.x.dx = 0;
  int86x(0x67, &r, &r, &rs);
  if (r.h.ah != 0x91) {
    printf("FAIL: Status2 = 0x%02x\n", r.h.ah);
    return 1;
  }
  printf("INFO: Status2 = feature not supported\n");

  r.x.ax = 0x5201;      // set attribute
  r.h.bl = 2;           // invalid attr
  r.x.dx = 0;
  int86x(0x67, &r, &r, &rs);
  if (r.h.ah != 0x90) {
    printf("FAIL: Status3 = 0x%02x\n", r.h.ah);
    return 1;
  }
  printf("INFO: Status3 = undefined attribute type\n");

  return 0;
}

""")

    results = self.runDosemu("testit.bat")

    self.assertIn("INFO: Status1 SUCCESS", results)
    self.assertIn("INFO: Status2 = feature not supported", results)
    self.assertIn("INFO: Status3 = undefined attribute type", results)

    self.assertNotIn("FAIL:", results)


def test_memory_ems_magic_20(self):
    """Memory EMS (EMS Magic) [20] Get Attribute Capability"""

    if getattr(self, 'noems', False):
        self.skipTest("no EMM installed")

    self.mkfile("testit.bat", BATCHFILE % 'ems_20', newline="\r\n")

    self.mkcom_with_ia16("ems_20", r"""

#include <i86.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char *argv[])
{
  union REGS r = {};
  struct SREGS rs;

  r.x.ax = 0x5202;      // get attribute capabilty
  r.x.dx = 0;
  int86x(0x67, &r, &r, &rs);
  if (r.h.ah != 0x00) {
    printf("FAIL: Status1 = 0x%02x\n", r.h.ah);
    return 1;
  }

  printf("INFO: Returned attribute capability (0x%02x)\n", r.h.al);

  return 0;
}

""")

    results = self.runDosemu("testit.bat")

    self.assertIn("INFO: Returned attribute capability (0x00)", results)

    self.assertNotIn("FAIL:", results)


def test_memory_ems_magic_21(self):
    """Memory EMS (EMS Magic) [21] Invalid Handle Name Subfunction"""

    if getattr(self, 'noems', False):
        self.skipTest("no EMM installed")

    self.mkfile("testit.bat", BATCHFILE % 'ems_21', newline="\r\n")

    self.mkcom_with_ia16("ems_21", r"""

#include <i86.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char *argv[])
{
  union REGS r = {};
  struct SREGS rs;

  r.x.ax = 0x5302;      // invalid handle name subfunction
  int86x(0x67, &r, &r, &rs);
  if (r.h.ah != 0x8f) {
    printf("FAIL: Status1 = 0x%02x\n", r.h.ah);
    return 1;
  }

  printf("INFO: Returned handle name subfunction\n");

  return 0;
}

""")

    results = self.runDosemu("testit.bat")

    self.assertIn("INFO: Returned handle name subfunction", results)

    self.assertNotIn("FAIL:", results)


def test_memory_ems_magic_88(self):
    """Memory EMS (EMS Magic) [88] Get Name For Each Handle"""

    if getattr(self, 'noems', False):
        self.skipTest("no EMM installed")

    self.mkfile("testit.bat", BATCHFILE % 'ems_88', newline="\r\n")

    self.mkcom_with_ia16("ems_88", r"""

#include <i86.h>
#include <stdio.h>
#include <string.h>

char name[9];

struct entry {
  uint16_t handle;
  uint16_t pages;
};

struct entry entries[256];


int main(int argc, char *argv[])
{
  union REGS r = {};
  struct SREGS rs;
  int i;

  r.x.ax = 0x4d00;      // Get All Handles Pages
  rs.es = FP_SEG(entries);
  r.x.di = FP_OFF(entries);
  int86x(0x67, &r, &r, &rs);

  if (r.h.ah != 0) {
    printf("FAIL: Status = 0x%02x\n", r.h.ah); // should be 0
    return 1;
  }

  for (i=0; i < r.x.bx; i++) {
    r.x.ax = 0x5300;      // Get Handle's Name
    r.x.dx = i;
    rs.es = FP_SEG(name);
    r.x.di = FP_OFF(name);
    int86x(0x67, &r, &r, &rs);
    if (r.h.ah != 0) {
      printf("FAIL: Status = 0x%02x\n", r.h.ah); // should be 0
      return 1;
    }
    name[8] = '\0';
    printf("INFO: Handle[%d] Name = '%s'\n", i, name);
  }

  return 0;
}

""")

    results = self.runDosemu("testit.bat")
    self.assertNotIn("FAIL:", results)
    self.assertIn("INFO: Handle[0] Name = 'SYSTEM  '", results)


def test_memory_ems_magic_89(self):
    """Memory EMS (EMS Magic) [89] Set Name On Handle"""

    if getattr(self, 'noems', False):
        self.skipTest("no EMM installed")

    self.mkfile("testit.bat", BATCHFILE % 'ems_89', newline="\r\n")

    self.mkcom_with_ia16("ems_89", r"""

#include <i86.h>
#include <stdio.h>
#include <string.h>

char new1[9] = "Groovy";
char name[9];

struct entry {
  uint16_t handle;
  uint16_t pages;
};

struct entry entries[256];


int main(int argc, char *argv[])
{
  union REGS r = {};
  struct SREGS rs;
  int i;
  uint16_t myhandle;

  r.x.ax = 0x4300;      // Allocate 100 Pages (function 4)
  r.x.bx = 100;
  int86x(0x67, &r, &r, &rs);
  if (r.h.ah != 0) {
    printf("FAIL: Status = 0x%02x\n", r.h.ah);
    return 1;
  }
  myhandle = r.x.dx;
  printf("INFO: Handle = 0x%04x\n", myhandle);

  r.x.ax = 0x5301;      // Set Handle's Name
  r.x.dx = myhandle;
  rs.ds = FP_SEG(new1);
  r.x.si = FP_OFF(new1);
  int86x(0x67, &r, &r, &rs);
  if (r.h.ah != 0) {
    printf("FAIL: Status = 0x%02x\n", r.h.ah);
    return 1;
  }

  r.x.ax = 0x4d00;      // Get All Handles Pages
  rs.es = FP_SEG(entries);
  r.x.di = FP_OFF(entries);
  int86x(0x67, &r, &r, &rs);

  if (r.h.ah != 0) {
    printf("FAIL: Status = 0x%02x\n", r.h.ah); // should be 0
    return 1;
  }

  for (i=0; i < r.x.bx; i++) {
    r.x.ax = 0x5300;      // Get Each Handle's Name
    r.x.dx = i;
    rs.es = FP_SEG(name);
    r.x.di = FP_OFF(name);
    int86x(0x67, &r, &r, &rs);
    if (r.h.ah != 0) {
      printf("FAIL: Status = 0x%02x\n", r.h.ah);
      return 1;
    }
    name[8] = '\0';
    printf("INFO: Handle[%d] Name = '%s'\n", i, name);
  }

  return 0;
}

""")

    results = self.runDosemu("testit.bat")
    self.assertIn("INFO: Handle[0] Name = 'SYSTEM  '", results)
    self.assertRegex(results, r"INFO: Handle\[\d+\] Name = 'Groovy'")
    self.assertNotIn("FAIL:", results)


def load_tests_memory_ems_magic(testcase):
    for name, func in list(globals().items()):
        if name.startswith('test_'):
            setattr(testcase, name, func)
            setattr(func, 'emstest', True)

    if 'emstest' not in testcase.attrs:
        testcase.attrs += ['emstest',]
