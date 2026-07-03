#!/usr/bin/env python3
"""Surface Infer findings to GitHub Actions.

Reads an Infer report.json and emits one ::error:: workflow command per
finding so each shows up as an inline annotation on the pull request, appends
a Markdown table to $GITHUB_STEP_SUMMARY when that variable is set, and exits
non-zero when any finding is present so the job fails after the findings have
been surfaced (rather than dying silently on a raw exit code).

An empty report ("[]") exits 0.

Self-test: python3 scripts/infer-annotate.py --selftest
"""

import json
import os
import sys


def escape(msg):
    # GitHub annotation messages are single-line; encode the separators the
    # workflow-command parser treats specially.
    return msg.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


def annotations(findings):
    out = []
    for f in findings:
        detail = f.get("qualifier") or f.get("bug_type") or "issue"
        out.append(
            "::error file=%s,line=%s,title=Infer %s::%s"
            % (
                f.get("file", ""),
                f.get("line", 0),
                f.get("bug_type", ""),
                escape(detail),
            )
        )
    return out


def summary(findings):
    if not findings:
        return "## Infer\n\nNo findings.\n"
    rows = [
        "## Infer findings (%d)\n" % len(findings),
        "| Type | Location | Detail |",
        "| --- | --- | --- |",
    ]
    for f in findings:
        detail = (f.get("qualifier", "") or "").replace("\n", " ").replace("|", "\\|")
        rows.append(
            "| %s | %s:%s | %s |"
            % (f.get("bug_type", "?"), f.get("file", "?"), f.get("line", 0), detail)
        )
    return "\n".join(rows) + "\n"


def selftest():
    sample = [
        {
            "file": "a.c",
            "line": 5,
            "bug_type": "NULL_DEREFERENCE",
            "qualifier": "x is\nnull",
        }
    ]
    ann = annotations(sample)
    assert ann[0].startswith("::error file=a.c,line=5,title=Infer "), ann
    assert "%0A" in ann[0] and "NULL_DEREFERENCE" in ann[0], ann
    assert "NULL_DEREFERENCE" in summary(sample)
    assert summary([]).strip().endswith("No findings.")
    assert annotations([]) == []
    print("selftest ok")
    return 0


def main(argv):
    if "--selftest" in argv:
        return selftest()
    path = argv[1] if len(argv) > 1 else "infer-out/report.json"
    if not os.path.exists(path):
        # The capture step failed before Infer wrote a report; that failure is
        # already surfaced, so do not add a traceback on top of it.
        print("::warning::%s missing; Infer produced no report" % path)
        return 0
    with open(path) as fh:
        findings = json.load(fh)

    for line in annotations(findings):
        print(line)

    step_summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if step_summary:
        with open(step_summary, "a") as fh:
            fh.write(summary(findings))

    if findings:
        print("Infer reported %d issue(s)" % len(findings), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
