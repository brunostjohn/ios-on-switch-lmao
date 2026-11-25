"""The optional external inputs must match the app before deploy opens the card."""

import hashlib
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tools.deploy import deploy


class CompilerInputsTest(unittest.TestCase):
    def test_changed_last_input_stops_deploy_before_opening_the_card(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            executable = root / 'overlay/Applications/SwitchHWTest.app/SwitchHWTest'
            executable.parent.mkdir(parents=True)
            pins = []
            for key in deploy.compiler_inputs.STATES:
                payload = b'SWAB0001' + (16).to_bytes(4, 'little') + key.encode()
                (root / (key + '.swab')).write_bytes(payload)
                pins.append(hashlib.sha256(payload).hexdigest().encode())
            executable.write_bytes(b'\0'.join(pins))
            build = root / 'pins.json'
            build.write_text('{"build":"24B119"}')
            args = deploy._parser().parse_args(
                [
                    'disk9',
                    '--boot-package',
                    str(root),
                    '--compiler-inputs',
                    str(root),
                    '--ipsw',
                    str(build),
                    '--spec',
                    's',
                ]
            )
            expected = deploy.compiler_inputs.load(root, root, '24B119')
            data = root / 'Data'
            deploy.compiler_inputs.stage(data, expected)
            for name, payload in expected.items():
                self.assertEqual((data / 'mobile/Media/Switch/benchloop/compiler' / name).read_bytes(), payload)
            last = root / (deploy.compiler_inputs.STATES[-1] + '.swab')
            last.write_bytes(last.read_bytes() + b'x')
            with (
                mock.patch.object(deploy.host, 'check_tools'),
                mock.patch.object(deploy.pull_logs, 'load_spec', return_value={}),
                mock.patch.object(deploy.cards, 'resolve') as resolve,
                self.assertRaisesRegex(ValueError, last.name),
            ):
                deploy._checked(args, mock.Mock())
            resolve.assert_not_called()


if __name__ == '__main__':
    unittest.main()
