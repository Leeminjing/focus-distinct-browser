"""Evidence-contract unit tests only; these never certify cross-user access."""
from copy import deepcopy
import unittest

from cross_user import validate_broker

NONCE = 'a' * 32
OWNER = {'user_sid': 'S-1-5-21-1-1001', 'logon_sid': 'S-1-5-5-1-1', 'authentication_id': '0:11'}
VALID = {'status': 'executed', 'expected_user_sid': 'S-1-5-21-1-1002', 'actual_process_sid': 'S-1-5-21-1-1002',
    'account_created': True, 'account_deleted': True, 'profile_removed': True, 'stage_removed': True,
    'probe': {'user_sid': 'S-1-5-21-1-1002', 'logon_sid': 'S-1-5-5-1-2', 'authentication_id': '0:12',
              'administrator': False, 'elevated': False, 'nonce': NONCE, 'control_readable': True,
              'endpoint_error': 5, 'pipe_error': 5}}


class EvidenceContract(unittest.TestCase):
    def test_complete_evidence_is_accepted(self):
        self.assertTrue(all(validate_broker(VALID, OWNER, NONCE).values()))

    def test_wrong_identity_or_local_netonly_token_is_rejected(self):
        for field, value in [('user_sid', OWNER['user_sid']), ('logon_sid', OWNER['logon_sid']),
                             ('authentication_id', OWNER['authentication_id']), ('administrator', True), ('elevated', True)]:
            with self.subTest(field=field):
                result = deepcopy(VALID); result['probe'][field] = value
                with self.assertRaises(ValueError): validate_broker(result, OWNER, NONCE)
        result = deepcopy(VALID); result['actual_process_sid'] = OWNER['user_sid']
        with self.assertRaises(ValueError): validate_broker(result, OWNER, NONCE)

    def test_missing_objects_busy_pipes_and_allowed_access_do_not_count_as_denial(self):
        for field in ('endpoint_error', 'pipe_error'):
            for code in (0, 2, 3, 231, None, '5', True):
                with self.subTest(field=field, code=code):
                    result = deepcopy(VALID); result['probe'][field] = code
                    with self.assertRaises(ValueError): validate_broker(result, OWNER, NONCE)

    def test_missing_control_stale_output_and_incomplete_cleanup_are_rejected(self):
        for field, value in [('nonce', 'b' * 32), ('control_readable', False)]:
            result = deepcopy(VALID); result['probe'][field] = value
            with self.assertRaises(ValueError): validate_broker(result, OWNER, NONCE)
        for field in ('account_created', 'account_deleted', 'profile_removed', 'stage_removed'):
            with self.subTest(field=field):
                result = deepcopy(VALID); result[field] = False
                with self.assertRaises(ValueError): validate_broker(result, OWNER, NONCE)
        result = deepcopy(VALID); result.update(status='not_run', probe=None)
        with self.assertRaises(ValueError): validate_broker(result, OWNER, NONCE)


if __name__ == '__main__': unittest.main()
