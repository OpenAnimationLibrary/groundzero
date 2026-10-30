"""CI-only contracts for bounded publisher transfers; no network is used."""

import importlib.util
import io
from pathlib import Path
import socket
import ssl
import tempfile
import unittest
import urllib.error
from unittest.mock import patch


SPEC = importlib.util.spec_from_file_location(
    "neural_evaluate", Path(__file__).resolve().parents[1] / "neural-style/evaluate.py")
EVALUATE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(EVALUATE)


class Response(io.BytesIO):
    def __init__(self, body=b"model", length=None):
        super().__init__(body)
        self.headers = {} if length is None else {"Content-Length": str(length)}


class InterruptedResponse(Response):
    def read(self, count):
        if self.tell():
            raise TimeoutError("interrupted body")
        return super().read(2)


class BoundedDownloadTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.target = Path(self.directory.name) / "artifact"

    def transfer(self, responses, limit=5):
        opener = patch.object(EVALUATE.urllib.request, "urlopen", side_effect=responses)
        sleeper = patch.object(EVALUATE.time, "sleep")
        return opener, sleeper, limit

    def test_timeout_before_body_recovers_without_partial_output(self):
        response = Response()
        opener, sleeper, limit = self.transfer([TimeoutError("headers"), response])
        with opener as opened, sleeper as slept:
            self.assertEqual(EVALUATE.download("https://example.invalid/model", self.target, limit), 5)
        self.assertEqual(opened.call_count, 2)
        slept.assert_called_once_with(2)
        self.assertEqual(self.target.read_bytes(), b"model")
        self.assertTrue(response.closed)
        self.assertFalse(self.target.with_suffix(".part").exists())

    def test_retryable_http_and_dns_failures_have_a_fixed_attempt_limit(self):
        for error in (urllib.error.HTTPError("https://example.invalid", 503, "unavailable", {}, None),
                      urllib.error.URLError(socket.gaierror(socket.EAI_AGAIN, "DNS retry"))):
            with self.subTest(error=type(error).__name__):
                opener, sleeper, limit = self.transfer([error, error, error])
                with opener as opened, sleeper as slept:
                    with self.assertRaises(type(error)):
                        EVALUATE.download("https://example.invalid/model", self.target, limit)
                self.assertEqual(opened.call_count, 3)
                self.assertEqual([call.args[0] for call in slept.call_args_list], [2, 4])
                self.assertFalse(self.target.exists())

    def test_permanent_http_dns_and_certificate_failures_are_not_retried(self):
        for error in (urllib.error.HTTPError("https://example.invalid", 404, "missing", {}, None),
                      urllib.error.URLError(socket.gaierror(socket.EAI_NONAME, "unknown host")),
                      urllib.error.URLError(ssl.SSLCertVerificationError("untrusted certificate"))):
            with self.subTest(error=type(error).__name__):
                opener, sleeper, limit = self.transfer([error])
                with opener as opened, sleeper as slept:
                    with self.assertRaises(type(error)):
                        EVALUATE.download("https://example.invalid/model", self.target, limit)
                self.assertEqual(opened.call_count, 1)
                slept.assert_not_called()

    def test_interrupted_body_is_not_downloaded_twice_or_published(self):
        self.target.write_bytes(b"existing")
        response = InterruptedResponse()
        opener, sleeper, limit = self.transfer([response])
        with opener as opened, sleeper as slept:
            with self.assertRaisesRegex(TimeoutError, "interrupted body"):
                EVALUATE.download("https://example.invalid/model", self.target, limit)
        self.assertEqual(opened.call_count, 1)
        slept.assert_not_called()
        self.assertTrue(response.closed)
        self.assertEqual(self.target.read_bytes(), b"existing")
        self.assertFalse(self.target.with_suffix(".part").exists())

    def test_declared_and_streamed_size_limits_remain_hard_failures(self):
        for response in (Response(length=6), Response(b"excess")):
            with self.subTest(headers=response.headers):
                opener, sleeper, limit = self.transfer([response])
                with opener as opened, sleeper as slept:
                    with self.assertRaisesRegex(RuntimeError, "exceeds download bound"):
                        EVALUATE.download("https://example.invalid/model", self.target, limit)
                self.assertEqual(opened.call_count, 1)
                slept.assert_not_called()
                self.assertTrue(response.closed)
                self.assertFalse(self.target.exists())
                self.assertFalse(self.target.with_suffix(".part").exists())


if __name__ == "__main__":
    unittest.main()
