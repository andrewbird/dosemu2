from os import makedirs, listdir
from shutil import rmtree

from common_framework import mkfile, mkexe


def ds2_set_fattrs(self, fstype, attr):
    testdir = "test-imagedir/dXXXXs/d"

    rmtree(testdir, ignore_errors=True)

    mkfile("testit.bat", """\
d:
c:\\setfattr
rem end
""", newline="\r\n")

    # compile sources
    mkexe("setfattr", r"""\
#include <dos.h>
#include <dir.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>


int main(void) {
  int ret;
  int handle;
  unsigned int oattr, nattr;

  const char *fname = "FOO.DAT";

  unlink(fname);

  ret = _dos_creatnew(fname, _A_NORMAL, &handle);
  if (ret != 0) {
     printf("FAIL: Create failed '%s', err=%d\n", fname, ret);
     return 1;
  }
  printf("INFO: Create = '%s', err=%d\n", fname, ret);

  ret =_dos_close(handle);
  if (ret != 0) {
     printf("FAIL: Close failed, err=%d\n", ret);
     return 1;
  }

  ret = _dos_getfileattr(fname, &oattr);
  if (ret != 0) {
     printf("FAIL: Getfattr(1) failed, err=%d\n", ret);
     return 1;
  }
  printf("INFO: Getfattr(1) attr=0x%02x\n", oattr);
  if (oattr & _A_XXX) {
     printf("FAIL: Getfattr(1) has hidden bit set\n");
     return 1;
  }

  ret = _dos_setfileattr(fname, oattr | _A_XXX);
  if (ret != 0) {
     printf("FAIL: Setfattr(1) failed, err=%d\n", ret);
     return 1;
  }
  printf("INFO: Setfattr success\n");

  ret = _dos_getfileattr(fname, &nattr);
  if (ret != 0) {
     printf("FAIL: Getfattr(2) failed, err=%d\n", ret);
     return 1;
  }
  printf("INFO: Getfattr(2) attr=0x%02x\n", nattr);
  if (nattr != (oattr | _A_XXX)) {
     printf("FAIL: Getfattr(2) expected (0x%02x), got (0x%02x)\n", (oattr | _A_XXX), nattr);
     return 1;
  }

  printf("PASS: results as expected\n");
  return 0;
}
""".replace('XXX', attr))

    makedirs(testdir)

    if fstype == "MFS":
        config="""\
$_hdimage = "dXXXXs/c:hdtype1 dXXXXs/d:hdtype1 +1"
$_floppy_a = ""
"""
    else:       # FAT
        files = [(x, 0) for x in listdir(testdir)]

        name = self.mkimage("12", files, bootblk=False, cwd=testdir)
        config="""\
$_hdimage = "dXXXXs/c:hdtype1 %s +1"
$_floppy_a = ""
""" % name

    results = self.runDosemu("testit.bat", config=config)

    self.assertNotIn("FAIL:", results)
