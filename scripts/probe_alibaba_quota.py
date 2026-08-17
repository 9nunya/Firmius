#!/usr/bin/env python3
"""Probe Alibaba Coding Plan quota using ~/.firmius/accounts/alibaba-token-plan.json.

The API key is read locally and never printed. Response fields that look secret
are redacted before output.
"""
from __future__ import annotations

import json
import sys
import urllib.error
import urllib.request
from pathlib import Path

ACCOUNT = Path.home() / ".firmius" / "accounts" / "alibaba-token-plan.json"


def redact(value):
    if isinstance(value, dict):
        out = {}
        for key, item in value.items():
            lower = key.lower()
            if any(word in lower for word in ("key", "token", "secret", "password", "cookie")):
                out[key] = "<redacted>"
            else:
                out[key] = redact(item)
        return out
    if isinstance(value, list):
        return [redact(item) for item in value]
    return value


def main() -> int:
    account = json.loads(ACCOUNT.read_text())
    credentials = account["credentials"]
    api_key = credentials["api_key"]
    region_name = credentials.get("region", "international")

    if region_name == "china":
        host = "https://bailian.console.aliyun.com/data/api.json"
        commodity = "sfm_codingplan_public_cn"
        current_region = "cn-beijing"
    else:
        host = "https://modelstudio.console.alibabacloud.com/data/api.json"
        commodity = "sfm_codingplan_public_intl"
        current_region = "ap-southeast-1"

    query = (
        "?action=zeldaEasy.broadscope-bailian.codingPlan."
        "queryCodingPlanInstanceInfoV2&product=broadscope-bailian"
        f"&api=queryCodingPlanInstanceInfoV2&currentRegionId={current_region}"
    )
    body = json.dumps({
        "queryCodingPlanInstanceInfoRequest": {
            "commodityCode": commodity,
            "onlyLatestOne": True,
        }
    }).encode()
    request = urllib.request.Request(
        host + query,
        data=body,
        method="POST",
        headers={
            "Accept": "application/json",
            "Content-Type": "application/json",
            "Authorization": f"Bearer {api_key}",
            "x-api-key": api_key,
            "X-DashScope-API-Key": api_key,
            "User-Agent": "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 Chrome/143 Safari/537.36",
        },
    )

    print(f"account={account['id']} region={region_name} endpoint={host}")
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            raw = response.read()
            print(f"status={response.status} content-type={response.headers.get('content-type')}")
    except urllib.error.HTTPError as error:
        raw = error.read()
        print(f"status={error.code} content-type={error.headers.get('content-type')}")
    except urllib.error.URLError as error:
        print(f"request-error={error.reason}", file=sys.stderr)
        return 2

    print(f"bytes={len(raw)}")
    try:
        payload = json.loads(raw)
    except json.JSONDecodeError:
        print(raw[:4000].decode("utf-8", "replace"))
        return 1
    print(json.dumps(redact(payload), indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
