<?php
/**
 * @file
 * Core Key Authentication functionality. Except queries, this is Drupal
 * independent.
 */

/**
 * @defgroup keyauth_db Database integration for KeyAuth - Drupal specific.
 * @{
 */

/**
 * Load a key.
 */
function keyauth_load($public_key) {
  ctools_include('export');
  $result = ctools_export_load_object('keyauth_keys', 'names', array('public_key' => $public_key));
  if (isset($result[$public_key])) {
    return (array) $result[$public_key];
  }
}

/**
 * Load all keys.
 */
function keyauth_all_keys() {
  ctools_include('export');
  ctools_export_load_object_reset('keyauth_keys');
  $configs = ctools_export_load_object('keyauth_keys', 'all');
  foreach ($configs as $k => $v) {
    $configs[$k] = (array)$v;
  }
  return $configs;
}

/**
 * Look up a private key.
 */
function keyauth_key($public_key) {
  $keys = keyauth_all_keys();
  foreach ($keys as $p => $key) {
    if ($p == $public_key) {
      return $key['private_key'];
    }
  }
}

/**
 * Save a key set, public key and private key will be generated.
 *
 * @param $key
 *   If $key is a string it will be interpreted as a title and a new key pair
 *   will be generated.
 *   If $key is an array it will be interpreted as a full key and saved as-is
 *   to the database. Users of this funciton are responsible for not
 *   accidentally overriding existing keys.
 *
 * @return
 *   Complete key set as an array.
 *
 * @todo Does not handle collisions on $public_key.
 */
function keyauth_save($key) {
  if (is_string($key)) {
    $save = array(
      'public_key' => keyauth_random('md5'),
      'private_key' => keyauth_random('sha1'),
      'title' => $key,
    );
  }
  else {
    $save = $key;
  }
  keyauth_delete($save['public_key']);
  drupal_write_record('keyauth_keys', $save);
  return keyauth_load($save['public_key']);
}

/**
 * Delete a key.
 */
function keyauth_delete($public_key) {
  db_query("DELETE FROM {keyauth_keys} WHERE public_key = '%s'", $public_key);
}

/**
 * Regenerate a private key for a given keyset.
 *
 * @param $public_key
 *   Public key.
 *
 * @return
 *   Complete key set as an array.
 */
function keyauth_regenerate($public_key) {
  $save = array(
    'public_key' => $public_key,
    'private_key' => keyauth_random('sha1'),
  );
  drupal_write_record('keyauth_keys', $save, 'public_key');
  return db_fetch_array(db_query("SELECT * FROM {keyauth_keys} WHERE public_key = '%s'", $public_key));
}

/**
 * Load from db.
 */
function keyauth_load_nonce($public_key, $nonce) {
  return db_result(db_query("SELECT nonce FROM {keyauth_timestamp_nonce} WHERE public_key = '%s' AND nonce = '%s'", $public_key, $nonce));
}

/**
 * Save nonce to db.
 *
 * @param $record
 *   Array of the pattern
 *
 *  $record = array(
 *    'timestamp' => time(),
 *    'public_key' => $public_key,
 *    'nonce' => $nonce,
 *  );
 */
function keyauth_save_nonce($record) {
  drupal_write_record('keyauth_timestamp_nonce', $record);
}

/**
 * @} End of "defgroup keyauth_db".
 */

/**
 * @defgroup keyauth_library KeyAuth library - Drupal independent.
 * @{
 */

/**
 * Sign a message.
 *
 * @param $public_key
 *   The public key identifying a private key.
 * @param $message
 *   A string that is the message to hash.
 *
 * @return
 *   An array with the following values:
 *   0 - A random unique nonce.
 *   1 - The timestamp denoting the validity of the nonce.
 *   2 - The hash of message, nonce and timestamp.
 */
function keyauth_sign($public_key, $message) {
  $nonce = keyauth_random();
  $timestamp = time();
  $hash = hash_hmac(KEYAUTH_ALGORITHM, $message . $nonce . $timestamp, keyauth_key($public_key));
  return array(
    $nonce,
    $timestamp,
    $hash,
  );
}

/**
 * Verify a message.
 */
function keyauth_verify($public_key, $message, $nonce, $timestamp, $hash) {
  if ($private_key = keyauth_key($public_key)) {
    if (_keyauth_verify_nonce($public_key, $nonce, $timestamp)) {
      return $hash == hash_hmac(KEYAUTH_ALGORITHM, $message . $nonce . $timestamp, $private_key);
    }
  }
  return FALSE;
}

/**
 * Sign a URL.
 *
 * @param $public_key
 *   The public key identifying the private key to sign this URL with. MUST be
 *   a valid public key.
 * @param $url
 *   The URL to sign. MUST NOT contain a query paramter 'hash', 'key', 'nonce'
 *   or 'timestamp.
 *
 * @return
 *   The signed URL. The signed URL can be verified once with
 *   keyauth_verify_url() on a system that has the same public key/private key
 *   pair configured.
 */
function keyauth_sign_url($public_key, $url) {
  // Normalize URL.
  list($scheme, $url, $fragment) = keyauth_normalize_url($url);
  // Sign.
  list($nonce, $timestamp, $hash) = keyauth_sign($public_key, $url);
  // Assemble.
  $delimiter = parse_url($url, PHP_URL_QUERY) ? '&' : '?';
  $url .= $delimiter . 'hash='. urlencode($hash) .'&key='. urlencode($public_key) .'&nonce='. urlencode($nonce) .'&timestamp='. $timestamp;
  if ($fragment) {
    $url .= "#$fragment";
  }
  return "$scheme://$url";
}

/**
 * Verify a URL.
 *
 * @param $url
 *   A URL that has been signed with keyauth_sign_url().
 *
 * @return
 *   TRUE if the URL's authenticity could be verified, FALSE otherwise.
 */
function keyauth_verify_url($url) {
  // Parse URL.
  $parsed = parse_url($url);
  if (!isset($parsed['query'])) {
    return FALSE;
  }
  parse_str($parsed['query'], $query);
  if (isset($query['hash']) && isset($query['key']) && isset($query['nonce']) && isset($query['timestamp'])) {
    $url = str_replace("hash={$query['hash']}&key={$query['key']}&nonce={$query['nonce']}&timestamp={$query['timestamp']}", '', $url);
    $hash = urldecode($query['hash']);
    $key = urldecode($query['key']);
    $nonce = urldecode($query['nonce']);
    $timestamp = $query['timestamp'];
    // Normalize URL.
    list($scheme, $url, $fragment) = keyauth_normalize_url($url);
    // Verify.
    return keyauth_verify($key, $url, $nonce, $timestamp, $hash);
  }
  else {
    return FALSE;
  }
}

/**
 * Normalize a URL for signing or verifying it. Scheme and fragments are not
 * being signed.
 *
 * @param $url
 *   The URL to be normalized.
 *
 * @return
 *   An array where the first item is the scheme, the second item is the URL
 *   without the scheme and the fragment and the third item is the fragment if
 *   there was one in the given URL.
 *
 * @todo Make RFC2396 compliant.
 * http://labs.apache.org/webarch/uri/rev-2002/rfc2396bis.html#comparison
 */
function keyauth_normalize_url($url) {
  if ($fragment = parse_url($url, PHP_URL_FRAGMENT)) {
    $url = str_replace("#$fragment", '', $url);
  }
  $scheme = parse_url($url, PHP_URL_SCHEME);
  $url = str_replace("$scheme://", '', $url);
  $url = trim($url, '?&');
  return array($scheme, $url, $fragment);
}


/**
 * Provide a random, time dependent string.
 */
function keyauth_random($algorithm = 'md5') {
  return hash($algorithm, uniqid(rand(), true));
}

/**
 * Verify nonce.
 */
function _keyauth_verify_nonce($public_key, $nonce, $timestamp) {
  if (keyauth_load_nonce($public_key, $nonce)) {
    return FALSE;
  }
  $time = time();
  if ($timestamp > $time + KEYAUTH_EXPIRY || $timestamp < $time - KEYAUTH_EXPIRY) {
    return FALSE;
  }
  $save = array(
    'timestamp' => time(),
    'public_key' => $public_key,
    'nonce' => $nonce,
  );
  keyauth_save_nonce($save);
  return TRUE;
}

/**
 * @} End of "defgroup keyauth_library".
 */
