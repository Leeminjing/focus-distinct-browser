"""Accounting/guard/cleanup contract tests; no browser launch or desktop input."""
import ctypes
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import uuid
from types import SimpleNamespace

from concurrency import guard, ResourceLimit, remove_profile, GIB, Case
from reporting import distribution, summarize
import windows_metrics as win


class Contracts(unittest.TestCase):
    def test_quantile_and_errors(self):
        self.assertEqual(distribution(range(1,21))['p95'],19)
        self.assertEqual(distribution([10,20])['median'],15)
        self.assertIsNone(distribution([])['p95'])
        result=summarize([{'operation':'observe','ok':True,'ms':5},
                          {'operation':'observe','ok':False,'ms':500}])['observe']
        self.assertEqual(result['p95'],5)
        self.assertEqual(result['error_rate'],.5)
        self.assertEqual(result['attempts'],2)

    def test_guard_no_browser_needed(self):
        guard({'total_physical_bytes':16*GIB,'available_physical_bytes':3*GIB,'available_commit_bytes':3*GIB})
        for available,commit in ((2*GIB,3*GIB),(3*GIB,GIB)):
            with self.assertRaises(ResourceLimit):
                guard({'total_physical_bytes':16*GIB,'available_physical_bytes':available,'available_commit_bytes':commit})

    def test_native_metrics_include_own_allocation(self):
        process=win.Process(os.getpid(),'client')
        try:
            before=process.sample()
            block=ctypes.create_string_buffer(8*1024**2)
            after=process.sample()
            self.assertEqual(before['birth_100ns'],after['birth_100ns'])
            self.assertTrue(after['alive'])
            self.assertGreaterEqual(after['private_bytes'],before['private_bytes']+6*1024**2)
            self.assertGreaterEqual(after['cpu_total_100ns'],before['cpu_total_100ns'])
            self.assertEqual(len(block),8*1024**2)
        finally:process.close()

    def test_preflight_blocks_launch_and_keeps_partial_report(self):
        with tempfile.TemporaryDirectory() as directory:
            args=SimpleNamespace(reserve_gib=2,reserve_fraction=.15,trace=False)
            with patch('concurrency.win.memory',return_value={'total_physical_bytes':16*GIB,
                    'available_physical_bytes':GIB,'available_commit_bytes':3*GIB}), \
                    patch('concurrency.subprocess.Popen') as launch:
                result=Case(args,20,3,Path(directory)/'case','unused').run()
            launch.assert_not_called()
            self.assertEqual(result['status'],'resource_guard')
            self.assertTrue((Path(directory)/'case/case.json').is_file())
            self.assertEqual(result['requests'],[])

    def test_process_tree_excludes_other_apps_and_reused_parent(self):
        class FakeProcess:
            def __init__(self,pid,kind,parent=0):
                self.pid,self.kind,self.parent=pid,kind,parent
                self.birth=50 if pid==6 else 100+pid
                self.path=Path('runtime.exe');self.alive=True
            def close(self):self.alive=False
        sampler=object.__new__(win.Sampler)
        sampler.tracked={1:FakeProcess(1,'host'),2:FakeProcess(2,'client')};sampler.runtime={}
        rows={3:{'parent':1,'name':'msedgewebview2.exe'},
              4:{'parent':999,'name':'msedgewebview2.exe'},
              5:{'parent':3,'name':'msedgewebview2.exe'},
              6:{'parent':1,'name':'msedgewebview2.exe'},
              7:{'parent':2,'name':'msedgewebview2.exe'}}
        with patch('windows_metrics.process_table',return_value=rows), \
                patch('windows_metrics.Process',FakeProcess),patch('windows_metrics.version',return_value='156'):
            sampler.discover()
        self.assertEqual(set(sampler.tracked),{1,2,3,5})

    def test_cleanup_exact_owned_uuid_only(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)/'DistinctBrowser'
            bid=str(uuid.uuid4()).upper();foreign=str(uuid.uuid4()).upper()
            target=root/'Profiles'/bid;target.mkdir(parents=True)
            other=root/'Profiles'/foreign;other.mkdir()
            (target/'data').write_text('test')
            endpoint=root/'Hosts'/str(uuid.uuid4()).upper()/'endpoint.json'
            remove_profile(endpoint,bid)
            self.assertFalse(target.exists());self.assertTrue(other.exists())
            with self.assertRaises(ValueError):remove_profile(endpoint,'../foreign')
            with self.assertRaises(ValueError):remove_profile(root/'other.json',foreign)
            with patch('concurrency.shutil.rmtree') as delete, patch('concurrency.os.walk',return_value=[(str(other),[],['link'])]):
                fake=unittest.mock.Mock(st_file_attributes=0x400,st_mode=0)
                with patch.object(Path,'lstat',return_value=fake), self.assertRaises(ValueError):
                    remove_profile(endpoint,foreign)
                delete.assert_not_called()


if __name__=='__main__':unittest.main()
