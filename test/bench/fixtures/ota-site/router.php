<?php
// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
//
// Bench update site: serves the SAME manifest fields the website's
// update/firmware.php?manifest=1 serves (version, size, sha256, url, hw,
// channel, source, release_id, released_at) and the image on ?app=1, from
// local release folders instead of GitHub. See README.md.
//
//   CF_OTA_RELEASES=<dir> php -S 0.0.0.0:8297 router.php
//
// A device whose `upd.base` is http://<host>:8297/<name> reads the release
// in <dir>/<name>/ (firmware.bin + release-info.json). Test knobs live in
// release-info.json only (the device never sees them):
//   "sha256"      manifest digest to announce (default: the real one)
//   "flip_offset" flip one byte at this offset when serving the image
//   "status"      answer every request for this release with this HTTP status

$root = getenv('CF_OTA_RELEASES');
$path = parse_url($_SERVER['REQUEST_URI'], PHP_URL_PATH);

// A minimal check-in (any credential) that only offers firmware, so the
// check-in's own manifest read can be exercised: /<name>/api/device-checkin.php
if (preg_match('~^/[A-Za-z0-9_-]{1,40}/api/device-checkin\.php$~', (string) $path)) {
    header('Content-Type: application/json; charset=utf-8');
    echo json_encode(['loadout_version' => 1, 'batch_id' => null, 'send_report' => false,
        'firmware' => ['offer' => true, 'url' => '/update/firmware.php?manifest=1'],
        'mode' => 'normal', 'next_poll_ms' => 86400000, 'server_time' => gmdate('Y-m-d\TH:i:s\Z')]);
    exit;
}
if (!is_string($root) || !preg_match('~^/([A-Za-z0-9_-]{1,40})/update/firmware\.php$~', $path, $m)) {
    http_response_code(404);
    exit;
}
$dir = rtrim($root, '/\\') . '/' . $m[1];
$bin = @file_get_contents($dir . '/firmware.bin');
$info = json_decode((string) @file_get_contents($dir . '/release-info.json'), true);
if ($bin === false || !is_array($info)) {
    http_response_code(502);
    header('Content-Type: application/json');
    echo json_encode(['error' => 'The update release is unavailable.']);
    exit;
}
if (isset($info['status'])) {
    http_response_code((int) $info['status']);
    header('Content-Type: application/json');
    echo json_encode(['error' => 'The update is temporarily unavailable.']);
    exit;
}
$sha = $info['sha256'] ?? hash('sha256', $bin);
$releaseId = (int) ($info['release_id'] ?? 1);
$channel = $info['channel'] ?? 'stable';

if (isset($_GET['app'])) {
    // A download URL is bound to the manifest that issued it (as on the site).
    if ((isset($_GET['release_id']) && $_GET['release_id'] !== (string) $releaseId) ||
        (isset($_GET['sha256']) && $_GET['sha256'] !== $sha)) {
        http_response_code(409);
        header('Content-Type: application/json');
        echo json_encode(['error' => 'The update changed. Please check for updates again.']);
        exit;
    }
    if (isset($info['flip_offset'])) {
        $at = (int) $info['flip_offset'];
        $bin[$at] = chr(ord($bin[$at]) ^ 0x01);
    }
    header('Content-Type: application/octet-stream');
    header('Content-Length: ' . strlen($bin));
    header('Cache-Control: no-cache');
    echo $bin;
    exit;
}
if (!isset($_GET['manifest'])) {
    http_response_code(404);
    exit;
}
$query = http_build_query(['app' => '1', 'tag' => 'v' . $info['version'], 'channel' => $channel,
    'release_id' => $releaseId, 'sha256' => $sha]);
header('Content-Type: application/json; charset=utf-8');
header('Cache-Control: no-cache');
echo json_encode([
    'version' => $info['version'], 'size' => strlen($bin), 'sha256' => $sha,
    'url' => '/update/firmware.php?' . $query,
    'hw' => $info['hw'] ?? ['min_rev' => '1.2', 'max_rev' => '1.2'],
    'channel' => $channel, 'source' => $info['source'] ?? 'official',
    'release_id' => $releaseId, 'released_at' => $info['released_at'],
]);
