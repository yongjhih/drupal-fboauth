<?php

/**
 * @file
 * Provides functions used during Facebook login processes.
 */


/**
 * Menu callback; The main page for processing OAuth login transactions.
 *
 * @param $action
 *   The action being requested. Currently supports the following:
 *    - connect: Initiate Facebook connection and log a user in.
 *    - deauth: Remove the exisitng Facebook connection access.
 */
function fboauth_action_page($action) {
  global $user;

  // TODO: Support loading of more than one App ID and App Secret.
  $app_id = variable_get('fboauth_id', '');
  $app_secret = variable_get('fboauth_secret', '');

  $action_name = $action['name'];

  $error_message = t('The Facebook login could not be completed due to an error. Please create an account or contact us directly. Details about this error have already been recorded to the error log.');

  if (!($app_id && $app_secret)) {
    watchdog('fboauth', 'A Facebook login was attempted but could not be processed because the module is not yet configured. Vist the <a href="!url">Facebook OAuth configuration</a> to set up the module.', array('!url' => url('admin/settings/fboauth')));
  }
  elseif (isset($_REQUEST['error'])) {
    $link = fboauth_action_link_properties($action_name, $app_id);
    watchdog('fboauth', 'A user refused to allow access to the necessary Facebook information (@permissions) to login to the site.', array('@permissions' => $link['query']['scope']));
    $error_message = t('This site requires access to information in order to log you into the site. If you like you can <a href="!facebook">sign in with Facebook again</a> or <a href="!register">register normally</a>.', array('!facebook' => url($link['href'], array('query' => $link['query'])), '!register' => url('user/register')));
  }
  elseif (!isset($_REQUEST['code'])) {
    watchdog('fboauth', 'A Facebook request code was expected but no authorization was received.');
  }
  // The primary action routine after access has been approved by the user.
  elseif ($access_token = fboauth_access_token($_REQUEST['code'], $action_name, $app_id, $app_secret)) {
    $destination = fboauth_action_invoke($action_name, $app_id, $access_token);
    if (empty($destination)) {
      $destination = isset($_REQUEST['destination']) ? $_REQUEST['destination'] : '<front>';
    }
    drupal_goto($destination);
  }

  // In the event of an error, we stay on this page.
  return $error_message;
}

/**
 * Invoke an action specified through hook_fboauth_action_info().
 */
function fboauth_action_invoke($action_name, $app_id, $access_token) {
  $action = fboauth_action_load($action_name);

  // Call the specified action.
  if (isset($action['callback'])) {
    $callback = $action['callback'];

    if (function_exists($callback)) {
      $callback($app_id, $access_token);
    }
  }
}

/**
 * Facebook OAuth callback for initiating a Facebook connection.
 */
function fboauth_action_connect($app_id, $access_token) {
  $fbuser = fboauth_graph_query('me', $access_token);
  $uid = fboauth_uid_load($fbuser->id);
  // If the user has logged in before, load their account and login.
  if ($uid && ($account = user_load($uid))) {
    user_external_login($account);
  }
  else {
    // If the user is already logged in, associate the two accounts.
    if (user_is_logged_in()) {
      fboauth_save($user->uid, $fbuser->id);
    }
    // Register a new user only if allowed.
    else if (variable_get('user_register', 1)) {
      $account = fboauth_create_user($fbuser);
      // Load the account fresh just to have a fully-loaded object.
      $account = user_load($account->uid);
      user_external_login($account);
    }
  }
}

/**
 * Facebook OAuth callback for deauthorizing the site from Facebook.
 */
function fboauth_action_deauth($app_id, $access_token) {
  fboauth_method_invoke('auth.revokeAuthorization', $access_token);
}

/**
 * Given a Facebook user object, associate or save a Drupal user account.
 */
function fboauth_create_user($fbuser) {
  // Use their Facebook user name (if defined), otherwise their real name.
  // If an account already exists with that name, increment until the namespace
  // is available.
  $username = empty($fbuser->username) ? $fbuser->name : $fbuser->username;
  $query = "SELECT uid FROM {users} WHERE name = '%s'";
  $uid = db_result(db_query($query, $username));
  $i = 0;
  while ($uid) {
    $i++;
    $uid = db_result(db_query($query, $username . $i));
  }
  if ($i > 0) {
    $username = $username . $i;
  }

  // Initialize basic properties that are unlikely to need changing.
  $edit = array(
    'name' => $username,
    'mail' => $fbuser->email,
    'status' => 1,
    'fboauth' => TRUE, // Used to signify this is being imported by FBOAuth.
    'fboauth_fbid' => $fbuser->id, // So that other modules can load the account.
  );

  // Profile module support.
  if (module_exists('profile')) {
    module_load_include('inc', 'fboauth', 'includes/fboauth.profile');
    fboauth_profile_create_user($edit, $fbuser);
  }

  // Allow other modules to manipulate the user information before save.
  foreach (module_implements('fboauth_user_presave') as $module) {
    $function = $module . '_fboauth_user_presave';
    $function($edit, $fbuser);
  }

  $account = user_save(NULL, $edit);

  // Retrieve the user's picture from Facebook and save it locally.
  if ($account->uid) {
    $path = file_create_path('pictures');
    file_check_directory($path, FILE_CREATE_DIRECTORY);
    $picture = file_get_contents('https://graph.facebook.com/' . $fbuser->id . '/picture?type=large');
    $picture_path = $path . '/picture-' . $account->uid . '.jpg';
    $file = fopen($picture_path, 'w');
    fwrite($file, $picture);
    fclose($file);

    // Check to make sure the picture isn't too large for the site settings.
    $picture_info = image_get_info($picture_path);
    list($max_dimensions['width'], $max_dimensions['height']) = explode('x', variable_get('user_picture_dimensions', '85x85'));
    if (image_get_toolkit() && $picture_info['width'] > $max_dimensions['width'] || $picture_info['height'] > $max_dimensions['height']) {
      image_scale($picture_path, $picture_path, $max_dimensions['width'], $max_dimensions['height']);
    }

    // Update the database record.
    db_query("UPDATE {users} SET picture = '%s' WHERE uid = %d", $picture_path, $account->uid);
  }

  // Allow other modules to manipulate the user information after save.
  foreach (module_implements('fboauth_user_save') as $module) {
    $function = $module . '_fboauth_user_save';
    $function($account, $fbuser);
  }

  return $account;
}

/**
 * Given an approval code from Facebook, return an access token.
 *
 * The approval code is generated by Facebook when a user grants access to our
 * site application to use their data. We use this approval code to get an
 * access token from Facebook. The access token usually is valid for about
 * 15 minutes, allowing us to pull as much information as we want about the
 * user.
 *
 * @param $code
 *   An approval code from Facebook. Usually pulled from the ?code GET parameter
 *   after a user has approved our application's access to their information.
 * @param $action_name
 *   The action is the directory name underneath the "fboauth" path. This value
 *   must be the same between the page originally provided to Facebook as the
 *   "redirect" URL and when requesting an access token.
 * @return
 *   An access token that can be used in REST queries against Facebook's Graph
 *   API, which will provide us with information about the Facebook user.
 */
function fboauth_access_token($code, $action_name, $app_id = NULL, $app_secret = NULL) {
  // Use the default App ID and App Secret if not specified.
  $app_id = isset($app_id) ? $app_id : variable_get('fboauth_id', '');
  $app_secret = isset($app_secret) ? $app_secret : variable_get('fboauth_secret', '');

  // Note that the "code" provided by Facebook is a hash based on the client_id,
  // client_secret, and redirect_url. All of these things must be IDENTICAL to
  // the same values that were passed to Facebook in the approval request. See
  // the fboauth_link_properties function.
  $query = array(
    'client_id' => $app_id,
    'client_secret' => $app_secret,
    'redirect_uri' => url('fboauth/' . $action_name, array('absolute' => TRUE, 'query' => !empty($_GET['destination']) ? array('destination' => $_GET['destination']) : array())),
    'code' => $code,
  );
  $token_url = url('https://graph.facebook.com/oauth/access_token', array('absolute' => TRUE, 'query' => $query));
  $authentication_result = drupal_http_request($token_url);

  if ($authentication_result->code != 200) {
    watchdog('fboauth', 'Facebook OAuth could not acquire an access token from Facebook. We queried the following URL: <code><pre>@url</pre></code>. Facebook\'s servers returned an error @error: <code><pre>@return</pre></code>', array('@url' => $token_url, '@error' => $authentication_result->code, '@return' => check_plain(print_r($authentication_result->data))));
  }
  else {
    // The result from Facebook comes back in a query-string-like format,
    // key1=value1&key2=value2. Parse into an array.
    $authentication_strings = explode('&', $authentication_result->data);
    $authentication_values = array();
    foreach ($authentication_strings as $authentication_string) {
      list($authentication_key, $authentication_value) = explode('=', $authentication_string);
      $authentication_values[$authentication_key] = $authentication_value;
    }
  }

  return $authentication_values['access_token'];
}

/**
 * Return a list of permissions based on a list of properties or connections.
 *
 * @param $access_requested
 *   Optional. A list of Facebook user properties or connections. If not
 *   specified, a list of all known permissions will be returned.
 * @return
 *   A list of Facebook permission names necessary to access those properties or
 *   connections.
 * @see http://developers.facebook.com/docs/reference/api/user/
 * @see http://developers.facebook.com/docs/authentication/permissions/
 */
function fboauth_user_permissions($access_requested = NULL) {
  $permissions = array();

  $properties = fboauth_user_properties();
  foreach ($properties as $property => $property_info) {
    if (isset($property_info['permission']) && (!isset($access_requested) || in_array($property, $access_requested))) {
      $permissions[$property_info['permission']] = $property_info['permission'];
    }
  }

  $connections = fboauth_user_connections();
  foreach ($connections as $connection => $connection_info) {
    if (isset($connection_info['permission']) && (!isset($access_requested) || in_array($connection, $access_requested))) {
      $permissions[$connection_info['permission']] = $connection_info['permission'];
    }
  }

  return $permissions;
}

/**
 * Return a list of Facebook user properties.
 *
 * This function provides a list of properties that may be attached directly to
 * a Facebook user account. This information is immediately available when a
 * user logs in with Facebook connect and may be stored locally.
 *
 * Each property requires extended permission granted by the end-user. The
 * returned array of properties provides the name of each required permission
 * and a human-readable name for the property.
 *
 * Note that access to a user's id, name, first_name, last_name, gender, locale,
 * link, username, third_party_id, timezone, updated_time, and verified
 * properties are always available if the user grants generic access to your
 * application.
 *
 * @param $include_common
 *   Optionally include all common properties in this list.
 *
 * @see fboauth_user_connections()
 * @see http://developers.facebook.com/docs/reference/api/user/
 * @see http://developers.facebook.com/docs/authentication/permissions/
 */
function fboauth_user_properties($include_common = FALSE) {
  $properties = array(
    'about' => array('permission' => 'user_about_me', 'label' => t('About me (a short bio)')),
    'bio' => array('permission' => 'user_about_me', 'label' => t('Biography')),
    'birthday' => array('permission' => 'user_birthday', 'label' => t('Birthday')),
    'education' => array('permission' => 'user_education_history', 'label' => t('Education history')),
    'email' => array('permission' => 'email', 'label' => t('E-mail')),
    'hometown' => array('permission' => 'user_hometown', 'label' => t('Hometown')),
    'location' => array('permission' => 'user_location', 'label' => t('Location')),
    'relationship_status' => array('permission' => 'user_relationships', 'label' => t('Relationship status (Single, Married, It\'s complicated, etc.)')),
    'interested_in' => array('permission' => 'user_relationship_details', 'label' => t('Interested in (Men, Women)')),
    'significant_other' => array('permission' => 'user_relationship_details', 'label' => t('Signficant other')),
    'political' => array('permission' => 'user_religion_politics', 'label' => t('Political view')),
    'quotes' => array('permission' => 'user_about_me', 'label' => t('Favorite quotes')),
    'religion' => array('permission' => 'user_religion_politics', 'label' => t('Religion')),
    'website' => array('permission' => 'user_website', 'label' => t('Website')),
    'work' => array('permission' => 'user_work_history', 'label' => t('Work history')),
  );

  if ($include_common) {
    $properties += array(
      'id' => array('label' => t('Facebook ID')),
      'name' => array('label' => t('Full name')),
      'first_name' => array('label' => t('First name')),
      'last_name' => array('label' => t('Last name')),
      'gender' => array('label' => t('Gender')),
      'locale' => array('label' => t('Locale')),
      'link' => array('label' => t('Facebook profile link')),
      'username' => array('label' => t('Username')),
      'third_party_id' => array('label' => t('3rd-party ID')),
      'timezone' => array('label' => t('Timezone')),
      'updated_time' => array('label' => t('Updated time')),
      'verified' => array('label' => t('Verified')),
    );
    ksort($properties);
  }

  return $properties;
}

/**
 * Return a list of Facebook connection points.
 *
 * This function provides a list of all of the Facebook GraphAPI connection
 * points that can be access to learn extended information about a user. Usually
 * each of these connection points will allow querying against content the user
 * has created as opposed to information directly about the user.
 *
 * Each connection requires extended permission granted by the end-user. The
 * returned array of connections provides the name of each required permission
 * and a human-readable name for the connection.
 *
 * Note that access to the "picture" and "friends" connections are always
 * allowed if the user grants generic access to your application.
 *
 * @see fboauth_user_properties()
 * @see http://developers.facebook.com/docs/reference/api/user/
 * @see http://developers.facebook.com/docs/authentication/permissions/
 */
function fboauth_user_connections() {
  return array(
    'accounts' => array('permission' => 'manage_pages', 'label' => t('Account pages')),
    'activities' => array('permission' => 'user_activities', 'label' => t('Activities')),
    'apprequests' => array('label' => t('App requests')), // No permission.
    'albums' => array('permission' => 'user_photos', 'label' => t('Photo albums')),
    'books' => array('permission' => 'user_likes', 'label' => t('Books liked')),
    'checkins' => array('permission' => 'user_checkins', 'label' => t('Check-ins')),
    'events' => array('permission' => 'user_events', 'label' => t('Events')),
    'feed' => array('permission' => 'read_stream', 'label' => t('User\'s wall')),
    'friendlists' => array('permission' => 'read_friendlists', 'label' => t('Lists of friends')),
    'home' => array('permission' => 'read_stream', 'label' => t('User\'s news feed')),
    'inbox' => array('permission' => 'read_mailbox', 'label' => t('Inbox threads')),
    'interests' => array('permission' => 'user_interests', 'label' => t('Interests')),
    'likes' => array('permission' => 'user_likes', 'label' => t('Pages liked')),
    'links' => array('permission' => 'read_stream', 'label' => t('Posted links')),
    'movies' => array('permission' => 'user_likes', 'label' => t('Movies liked')),
    'music' => array('permission' => 'user_likes', 'label' => t('Music liked')),
    'notes' => array('permission' => 'read_stream', 'label' => t('Notes')),
    'outbox' => array('permission' => 'read_mailbox', 'label' => t('Outbox')),
    'photos' => array('permission' => 'user_photos', 'label' => t('Photos')),
    'posts' => array('permission' => 'read_stream', 'label' => t('Posts')),
    'statuses' => array('permission' => 'read_stream', 'label' => t('Status updates')),
    'tagged' => array('permission' => 'read_stream', 'label' => t('Tagged in photos, videos, and posts')),
    'television' => array('permission' => 'user_likes', 'label' => t('Television liked')),
    'updates' => array('permission' => 'read_mailbox', 'label' => t('Inbox updates')),
    'videos' => array('permission' => 'user_videos', 'label' => t('Videos')),
  );
}

/**
 * Execute a Graph API query through Facebook.
 *
 * @see http://developers.facebook.com/docs/reference/api/
 */
function fboauth_graph_query($id, $access_token, $parameters) {
  $graph_url = url('https://graph.facebook.com/' . $id, array('absolute' => TRUE, 'query' => array('access_token' => $access_token)));
  $graph_result = drupal_http_request($graph_url);
  return json_decode($graph_result->data);
}

/**
 * Execute a legacy REST API method through Facebook.
 *
 * @see http://developers.facebook.com/docs/reference/rest/
 */
function fboauth_method_invoke($method, $access_token, $parameters = array()) {
  // Provide the default App ID for all requests if not specified.
  if (!isset($parameters['app_id'])) {
    $parameters['app_id'] = variable_get('fboauth_id', '');
  }

  // Keep all results in JSON for simplicity.
  $parameters['format'] = 'json';

  // Perform the REST query.
  $query_string = drupal_query_string_encode($parameters);
  $result = drupal_http_request('https://api.facebook.com/method/' . $method . '?' . $query_string);
  return json_decode($result->data);
}
