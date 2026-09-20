"""Job wire bounds, launch ambiguity, output bytes, and native lifecycle."""
import base64
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'mcp-server'))
from agent_client import AgentClient, AgentInputError, AgentProtocolError
from capabilities import describe, JOBS
from test_agent_client import FakeSocket
import server

ID='a'*32
STATUS=(f'id={ID}\nstate=running\npid=123\nexit_code=unknown\ncaptured_bytes=3\n'
        'output_limit=1048576\ntruncated=0\noutput_error=0\ncancel_scope=process\ncommand_mode=shell\n').encode()
def reply(payload):
    return FakeSocket(b'OK\nSIZE:'+str(len(payload)).encode()+b'\n'+payload)

class JobTests(unittest.TestCase):
    def client(self):
        return AgentClient('test.invalid',2222,'example',text_encoding='cp1252',exec_encoding='cp437')

    def test_start_status_list_and_binary_offsets(self):
        c=self.client();s=reply(STATUS)
        with patch.object(socket,'create_connection',return_value=s):
            data=c.job_start(ID,'echo caf\u00e9')
        self.assertIsNone(data['exit_code']);self.assertEqual(data['pid'],123)
        self.assertEqual(s.sent[1],f'JOBSTART {ID} S echo caf\u00e9\n'.encode('cp1252'))
        for method,args,payload,expected in [('job_list',(),(ID+'\n').encode(),[ID]),
                                             ('job_read',(ID,2,3),b'\0\xffx',b'\0\xffx')]:
            s=reply(payload)
            with patch.object(socket,'create_connection',return_value=s):self.assertEqual(getattr(c,method)(*args),expected)
        s=reply(STATUS)
        with patch.object(socket,'create_connection',return_value=s):self.assertEqual(c.job_status(ID)['state'],'running')

    def test_validation_and_malformed_responses(self):
        c=self.client()
        for method,args in [('job_status',('bad',)),('job_read',(ID,-1,2)),('job_read',(ID,0,65537)),('job_cancel',(ID+'\n',)),('job_release',('a',))]:
            with patch.object(socket,'create_connection') as connect:
                with self.assertRaises(AgentInputError):getattr(c,method)(*args)
                connect.assert_not_called()
        for payload in [STATUS+b'id='+ID.encode()+b'\n',STATUS.replace(b'state=running',b'state=success'),STATUS.replace(b'captured_bytes=3',b'captured_bytes=99999999'),STATUS.replace(b'exit_code=unknown',b'exit_code=-')]:
            s=reply(payload)
            with patch.object(socket,'create_connection',return_value=s):
                with self.assertRaises(AgentProtocolError):c.job_status(ID)
        s=FakeSocket(b'OK\nSIZE:65537\n')
        with patch.object(socket,'create_connection',return_value=s):
            with self.assertRaises(AgentProtocolError):c.job_read(ID)
        self.assertTrue(s.closed)

    def test_bridge_requires_advertised_support_and_never_retries_start(self):
        agent=Mock();agent.sysinfo.return_value={'os_family':'nt'}
        with patch.object(server,'_agent',return_value=agent):
            self.assertIn('does not advertise',server.legacy_job_start('test','program'))
        agent.job_start.assert_not_called()
        agent.sysinfo.return_value={'os_family':'nt','exec_jobs':'1'}
        agent.job_start.side_effect=TimeoutError('response timed out')
        with patch.object(server,'_agent',return_value=agent):data=json.loads(server.legacy_job_start('test','program'))
        self.assertTrue(data['launch_uncertain']);self.assertRegex(data['job_id'],r'^[a-f0-9]{32}$')
        agent.job_start.assert_called_once()
        self.assertTrue(JOBS <= set(describe(agent.sysinfo.return_value)['tools']))
        self.assertFalse(JOBS & set(describe({'os_family':'nt'})['tools']))

    def test_bridge_raw_output_survives_decode_failure(self):
        agent=Mock();agent.sysinfo.return_value={'exec_jobs':'1'};agent.exec_encoding='utf-8';agent.job_read.return_value=b'\xff\0'
        with patch.object(server,'_agent',return_value=agent):data=json.loads(server.legacy_job_output('test',ID,9,2))
        self.assertEqual(base64.b64decode(data['data_base64']),b'\xff\0');self.assertEqual(data['next_offset'],11)
        self.assertIn('decoding_error',data)
        agent.job_start.assert_not_called()

    def test_update_preserves_retained_jobs(self):
        agent=Mock();agent.job_list.return_value=[ID]
        with self.assertRaisesRegex(ValueError,'retained jobs'):
            server._check_update_jobs(agent,{'exec_jobs':'1'})
        agent.job_list.return_value=[]
        server._check_update_jobs(agent,{'exec_jobs':'1'})
        agent.reset_mock()
        server._check_update_jobs(agent,{})
        agent.job_list.assert_not_called()

    @unittest.skipUnless(os.name=='nt','native Win32 process/pipe test')
    def test_native_job_engine(self):
        compiler=os.environ.get('CC','gcc')
        with tempfile.TemporaryDirectory() as td:
            exe=Path(td)/'jobs.exe'
            subprocess.run([compiler,'-O2','-Wall','-march=i486','-DWIN32_LEAN_AND_MEAN',
                            str(ROOT/'agent-win32/tests/jobs_fixture.c'),'-o',str(exe),
                            '-static-libgcc','-luser32'],check=True)
            result=subprocess.run([str(exe)],capture_output=True,timeout=55)
            self.assertEqual(result.returncode,0,result.stdout.decode(errors='replace')+result.stderr.decode(errors='replace'))
            self.assertIn(b'PASS job engine',result.stdout)

if __name__=='__main__':unittest.main()
