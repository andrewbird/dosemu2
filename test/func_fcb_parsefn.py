
def fcb_parsefn(self):

    self.mkcom_with_ia16("fcbparse", r"""

#include <dos.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

  /*
    char _fcb_drive;
    char _fcb_name[8];
    char _fcb_ext[3];
    short _fcb_curblk;
    short _fcb_recsize;
    long _fcb_filsize;
    short _fcb_date;
    char _fcb_resv[10];
    char _fcb_currec;
    long _fcb_random;
  */
struct _fcb xfcb;

char buf[1024];

int main(int argc, char *argv[])
{
  union REGS r = {};
  struct SREGS sr;
  int handle;
  int ret, rc;
  unsigned char option;
  unsigned char retval;
  char *remainder;

  if (argc < 4) {
    printf("FAIL: Usage: argv[0] option argsfile rsltfile\n");
    return -1;
  }

  option = strtol(argv[1], NULL, 0);
  if (option != 0 && option != 1) {
    printf("FAIL: Only option bit 0, whitespace processing is supported\n");
    return -2;
  }
  printf("INFO: Option value is 0x%02x\n", option);

  ret = _dos_open(argv[2], O_RDONLY, &handle);
  if (ret != 0) {
    printf("FAIL: File '%s' not opened\n", argv[2]);
    return -3;
  }

  if (_dos_read(handle, buf, sizeof buf, &rc) != 0) {
    printf("FAIL: File '%s' not read\n", argv[1]);
    _dos_close(handle);
    return -1;
  }
  _dos_close(handle);

  memset(&xfcb, 0, sizeof(xfcb));

  segread(&sr);

  r.x.ax = 0x2901;
  sr.ds = FP_SEG(buf);
  r.x.si = FP_OFF(buf);
  sr.es = FP_SEG(&xfcb);
  r.x.di = FP_OFF(&xfcb);

  intdosx(&r, &r, &sr);

  retval = r.h.al;
  remainder = (char *)(uintptr_t)r.x.si;

  ret = _dos_creat(argv[3], 0, &handle);
  if (ret != 0) {
    printf("FAIL: File '%s' not opened\n", argv[3]);
    return -3;
  }

  if (_dos_write(handle, &retval, sizeof retval, &rc) != 0) {
    printf("FAIL: File '%s' retval not written\n", argv[3]);
    _dos_close(handle);
    return -4;
  }
  if (_dos_write(handle, &xfcb, sizeof xfcb, &rc) != 0) {
    printf("FAIL: File '%s' xfcb not written\n", argv[3]);
    _dos_close(handle);
    return -4;
  }
  if (_dos_write(handle, remainder, strlen(remainder), &rc) != 0) {
    printf("FAIL: File '%s' remainder not written\n", argv[3]);
    _dos_close(handle);
    return -4;
  }

  _dos_close(handle);

  printf("INFO: result is 0x%02x\n", retval);
  printf("INFO: drive is 0x%02x\n", xfcb._fcb_drive);
  printf("INFO: name is %.8s\n", xfcb._fcb_name);
  printf("INFO: ext is %.3s\n", xfcb._fcb_ext);
  printf("INFO: remainder is '%s'\n", remainder);

  return 0;
}

""")

    # option, cmdline, retval, FCB, remainder

    TESTS = (
# Bit 0: 0 = Do not skip leading separators
        (0x00, 'plain.fil',
            0x00, b'\x00PLAIN   FIL\x00\x00\x00\x00', ''),

        (0x00, ' plain.fil',
            0x00, b'\x00PLAIN   FIL\x00\x00\x00\x00', ''),  # odd still parsed as if there were no leading space
        (0x00, '  plain.fil',
            0x00, b'\x00PLAIN   FIL\x00\x00\x00\x00', ''),  # odd still parsed as if there were no leading spaces
        (0x00, ';plain.fil',
            0x00, b'\x00PLAIN   FIL\x00\x00\x00\x00', ''),  # odd still parsed as if there were no leading semicolon
#        (0x00, ';;plain.fil',
#            0x00, b'\x00           \x00\x00\x00\x00', ';plain.fil'),
        (0x00, '\\plain.fil',
            0x00, b'\x00           \x00\x00\x00\x00', '\\plain.fil'),
        (0x00, '\\\\plain.fil',
            0x00, b'\x00           \x00\x00\x00\x00', '\\\\plain.fil'),
        (0x00, ':plain.fil',
            0x00, b'\x00PLAIN   FIL\x00\x00\x00\x00', ''),
#        (0x00, '::plain.fil',
#            0x00, b'\x00           \x00\x00\x00\x00', ':plain.fil'),
        (0x00, '/plain.fil',
            0x00, b'\x00           \x00\x00\x00\x00', '/plain.fil'),
        (0x00, '//plain.fil',
            0x00, b'\x00           \x00\x00\x00\x00', '//plain.fil'),

        (0x00, 'a:single.fil',
            0x00, b'\x01SINGLE  FIL\x00\x00\x00\x00', ''),
        (0x00, 'a:\\single.fil',
            0x00, b'\x01           \x00\x00\x00\x00', '\\single.fil'),
        (0x00, 'c:\\subdir\\single.fil',
            0x00, b'\x03           \x00\x00\x00\x00', '\\subdir\\single.fil'),

        (0x00, 'file1.ex2 file2.ex4',
            0x00, b'\x00FILE1   EX2\x00\x00\x00\x00', ' file2.ex4'),
        (0x00, 'l: m:',
            0xff, b'\x0c           \x00\x00\x00\x00', ' m:'),
        (0x00, 'z:foo a:bar',
            0xff, b'\x1aFOO        \x00\x00\x00\x00', ' a:bar'),
        (0x00, 'z:foo.ex1 a:bar.ex2',
            0xff, b'\x1aFOO     EX1\x00\x00\x00\x00', ' a:bar.ex2'),
        (0x00, 'a:filename.bin c:filename.txt',
            0x00, b'\x01FILENAMEBIN\x00\x00\x00\x00', ' c:filename.txt'),

        (0x00, 'a:\\first.fil c:second.ext',
            0x00, b'\x01           \x00\x00\x00\x00', '\\first.fil c:second.ext'),
        (0x00, 'c:\\subdir\\first.fil d:second.fil',
            0x00, b'\x03           \x00\x00\x00\x00', '\\subdir\\first.fil d:second.fil'),


# Bit 0: 1 = Ignore/skip leading separators (such as spaces or semicolons); 
        (0x01, 'plain.fil',
            0x00, b'\x00PLAIN   FIL\x00\x00\x00\x00', ''),
        (0x01, ' plain.fil',
            0x00, b'\x00PLAIN   FIL\x00\x00\x00\x00', ''),
        (0x01, ';plain.fil',
            0x00, b'\x00PLAIN   FIL\x00\x00\x00\x00', ''),
#        (0x01, ';;plain.fil',
#            0x00, b'\x00           \x00\x00\x00\x00', ';plain.fil'),

# Wildcards
        (0x00, 'plain.*',
            0x01, b'\x00PLAIN   ???\x00\x00\x00\x00', ''),
        (0x00, '*.fil',
            0x01, b'\x00????????FIL\x00\x00\x00\x00', ''),
        (0x00, '*.*',
            0x01, b'\x00???????????\x00\x00\x00\x00', ''),
        (0x00, '*',
            0x01, b'\x00????????   \x00\x00\x00\x00', ''),
        (0x00, '*.',
            0x01, b'\x00????????   \x00\x00\x00\x00', ''),
#        (0x00, '.*',
#            0x01, b'\x00        ???\x00\x00\x00\x00', ''),
        (0x00, 'a*.fil',
            0x01, b'\x00A???????FIL\x00\x00\x00\x00', ''),
        (0x00, 'simple.f*',
            0x01, b'\x00SIMPLE  F??\x00\x00\x00\x00', ''),
        (0x00, 's??ple.fil',
            0x01, b'\x00S??PLE  FIL\x00\x00\x00\x00', ''),
        (0x00, 'simple.f?l',
            0x01, b'\x00SIMPLE  F?L\x00\x00\x00\x00', ''),
        (0x00, 's??.fil',
            0x01, b'\x00S??     FIL\x00\x00\x00\x00', ''),
        (0x00, 's.f?',
            0x01, b'\x00S       F? \x00\x00\x00\x00', ''),
        (0x00, 's?.f*',
            0x01, b'\x00S?      F??\x00\x00\x00\x00', ''),


# The tests below were run on MS-DOS 6.22 to provide the reference values, with which
# MS-DOS 7.0 and 7.1 agreed, however the usefulness of such truncated FCBs is very dubious.
#       (0x00, 'c:verylongfilename.ext d:second.fil',
#          0x00, b'\x03VERYLONGEXT\x00\x00\x00\x00', ' d:second.fil'),
#       (0x00, 'a:filename.bin c:verylongfilename.ext',
#          0x00, b'\x01FILENAMEBIN\x00\x00\x00\x00', ' c:verylongfilename.ext'),
    )

# input files called 'args_000.txt', content is command line.
# output files called 'rslt_000.bin', content is retval + FCB + remainder

    content = ''
    for index, (option, tstr, *_) in enumerate(TESTS):
        f = self.workdir / f'args_{index:03d}.txt'
        f.write_text(tstr)
        content += f'fcbparse 0x{option:02x} {f.name} rslt_{index:03d}.bin\n'
    content += 'rem end\n'
    self.mkfile("testit.bat", content, newline="\r\n")

    results = self.runDosemu("testit.bat")

    for index, (option, tstr, expval, expfcb, exprem) in enumerate(TESTS):
        f = self.workdir / f'rslt_{index:03d}.bin'
        try:
            b = f.read_bytes()
        except:
            raise self.failureException(f"Read error on '{f.name}'") from None

        val = b[0]
        fcb = b[1:17]
        rem = b[38:].decode('ascii')

        self.assertEqual(expval, val, f'''Test {index} failed to return correct value from "{tstr}"''')
        self.assertEqual(expfcb, fcb, f'''Test {index} failed to produce correct fcb from "{tstr}"''')
        self.assertEqual(exprem, rem, f'''Test {index} failed to leave correct remainder from "{tstr}"''')
