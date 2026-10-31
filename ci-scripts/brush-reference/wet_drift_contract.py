"""Original fixed-canvas, common-prefix paths for source drift measurement."""
CHECKPOINTS = (1, 10, 100, 1000, 10000)
WIDTH = HEIGHT = 2048
PITCH = 16
COLUMNS = 128


def center(index):
    row, column = divmod(index, COLUMNS)
    return [8 + PITCH * (column if row % 2 == 0 else COLUMNS - 1 - column), 8 + PITCH * row]


def path(count):
    if count not in CHECKPOINTS:
        raise ValueError("Unsupported bounded drift checkpoint")
    # Compress collinear vertices only. Each path is the exact prefix of the
    # same serpentine, with a one-pixel terminal segment below the dab pitch.
    vertices = [center(0)]
    for index in range(1, count):
        current, previous = center(index), center(index - 1)
        if index > 1:
            before = center(index - 2)
            if (current[0] - previous[0], current[1] - previous[1]) == (previous[0] - before[0], previous[1] - before[1]):
                vertices[-1] = current
                continue
        vertices.append(current)
    final, following = center(count - 1), center(count)
    vertices.append([final[0] + (following[0] - final[0]) // PITCH,
                     final[1] + (following[1] - final[1]) // PITCH])
    return vertices


def validate(case):
    spec = case.get("driftProbe", {})
    count = spec.get("nominalDabs")
    if (count not in CHECKPOINTS or case.get("sourcePath") != path(count) or
            case.get("diameter") != 8 or case.get("spacing") != 2 or
            type(spec.get("calibration")) is not bool or
            case.get("sourceEngine") != ("paintbrush" if spec["calibration"] else "colorsmudge")):
        raise ValueError("Drift path differs from bounded common-prefix contract")


def initial():
    # Original alternating red/blue strips with a continuous alpha ramp.
    rows = []
    for y in range(HEIGHT):
        alpha = min(255, y * 255 // 1280)
        red, blue = bytes((0, 0, 255, alpha)), bytes((255, 0, 0, alpha))
        rows.append(b"".join((red if (x // 64) % 2 == 0 else blue) * 64
                             for x in range(0, WIDTH, 64)))
    return b"".join(rows)


def capture(before, after, case):
    validate(case)
    if len(before) != WIDTH * HEIGHT * 4 or len(after) != len(before):
        raise ValueError("Invalid fixed drift raster")
    count = case["driftProbe"]["nominalDabs"]
    calibration = case["driftProbe"]["calibration"]
    changed = set()
    max_distance = 0
    for offset in range(0, len(after), 4):
        if before[offset:offset + 4] == after[offset:offset + 4]:
            continue
        if not before[offset + 3] and not after[offset + 3]:
            continue  # Preserve raw hashes; invisible RGB is not a deposit.
        pixel = offset // 4
        y, x = divmod(pixel, WIDTH)
        row, column = y // PITCH, x // PITCH
        index = row * COLUMNS + (column if row % 2 == 0 else COLUMNS - 1 - column)
        cx, cy = center(index)
        max_distance = max(max_distance, abs(x - cx), abs(y - cy))
        changed.add(index)
    expected = set(range(count)) if calibration else set(range(1, count))
    if changed != expected or max_distance > 5:
        raise ValueError("Source drift deposited missing, extra or misplaced cells: " +
                         repr({"nominal": count, "missing": sorted(expected - changed)[:8],
                               "extra": sorted(changed - expected)[:8], "maxDistance": max_distance}))
    alphas = []
    for index in range(count):
        x, y = center(index)
        alphas.append(after[(y * WIDTH + x) * 4 + 3])
    if calibration and (not alphas or alphas[0] not in (127, 128) or len(set(alphas)) != 1):
        raise ValueError("Dry buildup count control shows duplicate/unequal deposits")
    return {"nominalDabs": count, "observedDepositCells": len(changed),
            "firstCellUnchanged": 0 not in changed, "maximumCellOffset": max_distance,
            "calibration": calibration, "calibrationAlpha": alphas[0] if calibration else None,
            "countStatus": "observable cells validated; hidden internal dab count not exposed",
            "singleContinuousStroke": True}
