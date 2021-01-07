def perms_check_rmdir(self, fstype, fntype):
    testdir = self.mkworkdir('d')

    lfn = {
        "SFN": "n",
        "LFN": "y",
    }[fntype]

    self.mkfile("testit.bat", """\
d:
set LFN=%s
c:\\defattr
rem end
""" % lfn, newline="\r\n")

    # compile sources
    self.mkexe_with_djgpp("defattr", r"""\
#include <dos.h>
#include <dir.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>


int main(int argc, char *argv[]) {
  int ret;
  unsigned int oattr;

  const char *dname_ro = "DIR_RO";
  const int dattr_ro = _A_SUBDIR | _A_RDONLY;

  // RO directory
  ret = mkdir(dname_ro, FA_RDONLY);
  if (ret != 0) {
    printf("FAIL: Mkdir failed '%s', err=%d\n", dname_ro, ret);
    return 1;
  }
  printf("INFO: Mkdir = '%s', err=%d\n", dname_ro, ret);

  ret = _dos_getfileattr(dname_ro, &oattr);
  if (ret != 0) {
    printf("FAIL: Getfattr failed, err=%d\n", ret);
    return 1;
  }
  printf("INFO: Getfattr attr=0x%02x\n", oattr);

  if (oattr != dattr_ro) {
    printf("FAIL: Default RO directory attr(0x%02x) != expected(0x%02x)\n", oattr, dattr_ro);
    return 1;
  }

  ret = rmdir(dname_ro);
  if (ret != 0) {
    printf("FAIL: Rmdir failed, err=%d\n", ret);
    return 1;
  }

  printf("PASS: results as expected\n");
  return 0;
}
""")

    if fstype == "MFS":
        config="""\
$_hdimage = "dXXXXs/c:hdtype1 dXXXXs/d:hdtype1 +1"
$_floppy_a = ""
"""
    else:       # FAT
        name = self.mkimage("12", cwd=testdir)
        config="""\
$_hdimage = "dXXXXs/c:hdtype1 %s +1"
$_floppy_a = ""
""" % name

    results = self.runDosemu("testit.bat", config=config)

    self.assertNotIn("FAIL:", results)
