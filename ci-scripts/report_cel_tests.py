"""Publish failed CTest output as annotations accessible with the checks API."""
import pathlib
import sys
import xml.etree.ElementTree as ET


def escape(value):
    return value.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


report = pathlib.Path(sys.argv[1])
if report.exists():
    for case in ET.parse(report).getroot().iter("testcase"):
        if case.find("failure") is None and case.get("status") != "fail":
            continue
        output = case.findtext("system-out", default="No test output recorded")
        print("::error title=" + escape(case.get("name", "Cel effect test"))
              + "::" + escape(output[-12000:]))
