"""Isolated negative paths; never contacts Explorer or Microsoft servers."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

exe = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix='tabtail-symbols-') as temp:
    cache = Path(temp) / 'symbol cache 中文'
    data = Path(temp) / 'data'
    def run(*args, code=0):
        p = subprocess.run([str(exe), '--data-dir', str(data), *args], capture_output=True, encoding='utf-8', timeout=25)
        assert p.returncode == code, (args, p.returncode, p.stdout, p.stderr)
        return json.loads(p.stdout)
    status = run('symbols', 'status', '--cache-dir', str(cache))
    assert status['network_used'] is False and not cache.exists()
    assert status['capabilities']['com_observation'] is True
    report = run('symbols', 'prepare', '--offline', '--cache-dir', str(cache), code=5)
    assert report['prepared'] is False and report['complete'] is False
    assert len(report['modules']) == 3
    assert all('Offline PDB' in m.get('error', '') or m.get('prepared') is False for m in report['modules'].values())
    assert any('Offline PDB' in m.get('error', '') for m in report['modules'].values())
    assert report['network_used'] is False
    assert len(status['xaml_modules']) == 2
    assert not data.exists(), 'Symbol operations must not rewrite config or agent state'
    assert not list(cache.glob('result-*.json')), 'Worker result files must be removed'
    assert not list(cache.glob('*/stage-*')), 'Staging directory must be removed after failure'
    # Invalid offline input for a different module must not create a manifest,
    # leak its module-specific staged PDB, or affect config/agent state.
    worker = exe.with_name('tabtail-symbols.exe')
    invalid = Path(temp) / 'invalid.pdb'
    invalid.write_bytes(b'not a valid Microsoft PDB')
    p = subprocess.run([str(worker), '--module', 'ui', '--offline', '--cache-dir', str(cache), '--pdb', str(invalid)], capture_output=True, encoding='utf-8', timeout=25)
    assert p.returncode == 5 and json.loads(p.stdout)['prepared'] is False
    assert not list(cache.glob('*/profile.json'))
    assert not list(cache.glob('*/stage-*'))
    assert not data.exists()
print('Isolated three-module status, offline failures and staged PDB cleanup passed')
