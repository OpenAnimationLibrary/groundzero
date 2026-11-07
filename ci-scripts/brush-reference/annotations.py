"""Lossless, byte-bounded GitHub annotation batches for source evidence."""
import json


def report_records(title, records):
    prefix = "::notice title=" + title + "::"

    def encoded(rows):
        text = json.dumps(rows, separators=(",", ":"))
        return text.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")

    def fits(text):
        return len(text.encode("utf-8")) <= 3900 and len((prefix + text).encode("utf-8")) < 4096

    batch = []
    for record in records:
        text = encoded(batch + [record])
        if not fits(text):
            if batch:
                print(prefix + encoded(batch))
                batch = []
            text = encoded([record])
            if not fits(text):
                raise RuntimeError("One evidence record exceeds the annotation byte limit: " + title)
        batch.append(record)
    if batch:
        print(prefix + encoded(batch))
