import unittest
from check_debug_credentials import inspect_file


class DebugCredentialGuardTests(unittest.TestCase):
    def test_private_files_are_rejected_even_when_force_added(self):
        for path in ('local/session.txt', 'host.dpapi', 'config/credentials.json', '.env.local', 'runtime-signaling-host/record.txt'):
            with self.subTest(path=path):
                self.assertTrue(inspect_file(path, b'fixture'))

    def test_template_requires_private_value(self):
        self.assertTrue(inspect_file('scripts/session/host.conf', b'session_code=UNIT0001'))
        self.assertFalse(inspect_file('scripts/session/host.conf', b'session_code=\n'))

    def test_documented_command_must_not_publish_a_live_code(self):
        self.assertTrue(inspect_file('docs/run.md', b'run.ps1 -SessionCode UNIT0001'))
        self.assertFalse(inspect_file('docs/run.md', b'run.ps1 -SessionCode $sessionCode'))

    def test_literal_password_is_rejected_without_returning_it(self):
        candidate = 'isolated-fixture-password'
        result = inspect_file('scripts/start.ps1', f"$Password = '{candidate}'".encode())
        self.assertTrue(result)
        self.assertNotIn(candidate, repr(result))

    def test_runtime_secret_variable_is_allowed(self):
        self.assertFalse(inspect_file('scripts/start.ps1', b'$Password = "$privateValue"'))

    def test_prefixed_default_and_commented_code_are_rejected(self):
        self.assertTrue(inspect_file('scripts/config.ps1', b"$script:CrossLanIntegrationSessionCode = 'UNIT0001'"))
        self.assertTrue(inspect_file('docs/run.md', b'# session_code=UNIT0001'))

    def test_isolated_test_values_are_allowed_but_private_files_are_not(self):
        self.assertFalse(inspect_file('tests/auth_tests.cpp', b'session_code=UNIT0001'))
        self.assertTrue(inspect_file('tests/fixture.dpapi', b'fixture'))


if __name__ == '__main__':
    unittest.main()
