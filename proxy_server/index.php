<?php
// Firebase Realtime Database Host
$firebase_host = "microplux-anti-theft-app-2026-default-rtdb.firebaseio.com";
$request_uri = $_SERVER['REQUEST_URI'];
$firebase_url = "https://" . $firebase_host . $request_uri;

$method = $_SERVER['REQUEST_METHOD'];
$headers = [
    "Content-Type: application/json"
];

$ch = curl_init();
curl_setopt($ch, CURLOPT_URL, $firebase_url);
curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
curl_setopt($ch, CURLOPT_CUSTOMREQUEST, $method);

// Request payload (POST, PATCH, PUT)
if ($method === 'POST' || $method === 'PATCH' || $method === 'PUT') {
    $body = file_get_contents('php://input');
    curl_setopt($ch, CURLOPT_POSTFIELDS, $body);
    $headers[] = "Content-Length: " . strlen($body);
}

curl_setopt($ch, CURLOPT_HTTPHEADER, $headers);
curl_setopt($ch, CURLOPT_SSL_VERIFYPEER, false); // SSL verification bypass

$response = curl_exec($ch);
$status = curl_getinfo($ch, CURLINFO_HTTP_CODE);
$curl_error = curl_error($ch);
curl_close($ch);

// If cURL fails (e.g. host blocks outbound, or domain resolution failed)
if ($response === false || $status === 0) {
    http_response_code(502);
    header("Content-Type: application/json");
    echo json_encode([
        "error" => "Bad Gateway",
        "message" => "Outbound cURL request to Firebase failed.",
        "curl_error" => $curl_error
    ]);
    exit;
}

http_response_code($status);
header("Content-Type: application/json");
echo $response;
?>
