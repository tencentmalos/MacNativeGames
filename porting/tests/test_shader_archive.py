# SPDX-License-Identifier: MIT
import importlib.util
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).parents[2]
spec=importlib.util.spec_from_file_location('shader_archive',ROOT/'porting/tools/shader_archive.py')
archive=importlib.util.module_from_spec(spec);spec.loader.exec_module(archive)


class ArchiveTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work=tempfile.TemporaryDirectory();base=Path(cls.work.name)
        cpp=base/'probe.cpp';cls.probe=base/'probe'
        cpp.write_text('''#include "shader_archive.h"
int main(int argc,char** argv){try{
 mng::shader::Archive a(argv[1]);std::vector<uint8_t> bytes;
 if(argc>2)return 2;
 if(a.count()!=2 || !a.read("0123456789abcdef.xgs",bytes) || bytes.size()!=32)return 3;
 for(unsigned i=0;i<32;i++)if(bytes[i]!=i)return 4;
 if(!a.read("0123456789abcdef.xgv",bytes) || bytes!=std::vector<uint8_t>(16,98))return 5;
 return a.read("ffffffffffffffff.xgs",bytes)?6:0;
}catch(const std::exception&){return argc>2?0:1;}}
''')
        subprocess.run([shutil.which('c++') or 'c++','-std=c++17','-I'+str(ROOT/'porting/nvn_vulkan'),str(cpp),'-o',str(cls.probe)],check=True)

    @classmethod
    def tearDownClass(cls):cls.work.cleanup()

    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.base=Path(self.tmp.name);self.source=self.base/'source';self.source.mkdir()
        (self.source/'0123456789abcdef.xgs').write_bytes(bytes(range(32)))
        (self.source/'0123456789abcdef.xgv').write_bytes(b'b'*16)
        (self.source/'unrelated.json').write_text('{}')
        self.output=self.base/'bundle.mngsa';archive.pack(self.source,self.output)

    def test_exact_roundtrip_and_missing(self):
        subprocess.run([str(self.probe),str(self.output)],check=True)

    def test_no_overwrite(self):
        before=self.output.read_bytes()
        with self.assertRaises(FileExistsError):archive.pack(self.source,self.output)
        self.assertEqual(self.output.read_bytes(),before)

    def corrupt(self,change):
        data=bytearray(self.output.read_bytes());change(data);bad=self.base/'bad';bad.write_bytes(data)
        subprocess.run([str(self.probe),str(bad),'expect-invalid'],check=True)

    def test_truncation(self):self.corrupt(lambda b:b.__delitem__(slice(100,None)))
    def test_offset_overflow(self):self.corrupt(lambda b:struct.pack_into('<Q',b,56,2**64-1))
    def test_index_extent(self):self.corrupt(lambda b:struct.pack_into('<I',b,12,200001))
    def test_duplicate_names(self):self.corrupt(lambda b:b.__setitem__(slice(72,96),b[32:56]))
    def test_invalid_name(self):self.corrupt(lambda b:b.__setitem__(52,ord('x')))
    def test_overlapping_payload(self):self.corrupt(lambda b:struct.pack_into('<Q',b,96,112))
    def test_empty_directory(self):
        empty=self.base/'empty';empty.mkdir()
        with self.assertRaises(ValueError):archive.pack(empty,self.base/'unused')


if __name__=='__main__':unittest.main()
