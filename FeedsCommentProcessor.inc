<?php

/**
 * @file
 * Class definition of FeedsCommentProcessor.
 */

// Create or delete FEEDS_COMMENT_BATCH_SIZE at a time.
define('FEEDS_COMMENT_BATCH_SIZE', 50);

// Deprecated. Use FEEDS_SKIPE_EXISTING, FEEDS_REPLACE_EXISTNG,
// FEEDS_UPDATE_EXISTING instead.
define('FEEDS_COMMENT_SKIP_EXISTING', 0);
define('FEEDS_COMMENT_REPLACE_EXISTING', 1);
define('FEEDS_COMMENT_UPDATE_EXISTING', 2);

/**
 * Creates comments from feed items.
 */
class FeedsCommentProcessor extends FeedsProcessor {

  /**
   * Implementation of FeedsProcessor::process().
   */
  public function process(FeedsImportBatch $batch, FeedsSource $source) {

    // Keep track of processed items in this pass.
    $processed = 0;
    if (!$batch->getTotal(FEEDS_PROCESSING)) {
      $batch->setTotal(FEEDS_PROCESSING, $batch->getItemCount());
    }

    while ($item = $batch->shiftItem()) {

      // Create/update if item does not exist or update existing is enabled.
      if (!($cid = $this->existingItemId($batch, $source)) || ($this->config['update_existing'] != FEEDS_SKIP_EXISTING)) {
        // Only proceed if item has actually changed.
        $hash = $this->hash($item);
        if (!empty($cid) && $hash == $this->getHash($cid)) {
          continue;
        }

        $comment = $this->buildComment($cid, $source->feed_nid);
        $comment->feeds_comment_item->hash = $hash;

        // Map and save comment. If errors occur don't stop but report them.
        try {
          $this->map($batch, $comment);
          if (empty($comment->nid)) {
            throw new Exception("Unable create comment with empty NID");
          }
          if ($this->config['authorize']) {
            $account = user_load($comment->uid);
            if (!user_access('post comments')) {
              throw new Exception('User ' . $account->uid . ' not authorized to post comments.');
            }
          }
          $node = node_load($comment->nid);
          if ($node->comment != 2) {
            throw new Exception('Comments are not allowed for this node.');
          }
          _feeds_comment_save((array)$comment);
          if (!empty($cid)) {
            $batch->updated++;
          }
          else {
            $batch->created++;
          }
        }
        catch (Exception $e) {
          drupal_set_message($e->getMessage(), 'warning');
          watchdog('feeds', $e->getMessage(), array(), WATCHDOG_WARNING);
        }
      }

      $processed++;
      if ($processed >= variable_get('feeds_comment_batch_size', FEEDS_COMMENT_BATCH_SIZE)) {
        $batch->setProgress(FEEDS_PROCESSING, $batch->created + $batch->updated);
        return;
      }
    }

    // Set messages.
    if ($batch->created) {
      drupal_set_message(format_plural($batch->created, 'Created @number comment', 'Created @number comments.', array('@number' => $batch->created,)));
    }
    elseif ($batch->updated) {
      drupal_set_message(format_plural($batch->updated, 'Updated @number comment.', 'Updated @number comments.', array('@number' => $batch->updated,)));
    }
    else {
      drupal_set_message(t('There are no new comments.'));
    }
    $batch->setProgress(FEEDS_PROCESSING, FEEDS_BATCH_COMPLETE);
  }

  /**
   * Implementation of FeedsProcessor::clear().
   */
  public function clear(FeedsBatch $batch, FeedsSource $source) {
    if (!$batch->getTotal(FEEDS_CLEARING)) {
      $total = db_result(db_query("SELECT COUNT(cid) FROM {feeds_comment_item} WHERE id = '%s' AND feed_nid = %d", $source->id, $source->feed_nid));
      $batch->setTotal(FEEDS_CLEARING, $total);
    }
    $result = db_query_range("SELECT cid FROM {feeds_comment_item} WHERE id = '%s' AND feed_nid = %d", $source->id, $source->feed_nid, 0, variable_get('feeds_comment_batch_size', FEEDS_COMMENT_BATCH_SIZE));
    while ($comment = db_fetch_object($result)) {
      _feeds_comment_delete($comment->cid);
      $batch->deleted++;
    }
    if (db_result(db_query_range("SELECT cid FROM {feeds_comment_item} WHERE id = '%s' AND feed_nid = %d", $source->id, $source->feed_nid, 0, 1))) {
      $batch->setProgress(FEEDS_CLEARING, $batch->deleted);
      return;
    }

    // Set message.
    drupal_get_messages('status');
    if ($batch->deleted) {
      drupal_set_message(format_plural($batch->deleted, 'Deleted @number comment.', 'Deleted @number comments.', array('@number' => $batch->deleted)));
    }
    else {
      drupal_set_message(t('There is no content to be deleted.'));
    }
    $batch->setProgress(FEEDS_CLEARING, FEEDS_BATCH_COMPLETE);
  }

  /**
   * Implement expire().
   */
  public function expire($time = NULL) {
    if ($time === NULL) {
      $time = $this->expiryTime();
    }
    if ($time == FEEDS_EXPIRE_NEVER) {
      return;
    }
    $result = db_query_range("SELECT c.cid FROM {comments} c INNER JOIN {feeds_comment_item} fci ON c.cid = fni.nid WHERE fci.id = '%s' AND c.timestamp < %d", $this->id, FEEDS_REQUEST_TIME - $time, 0, variable_get('feeds_comment_batch_size', FEEDS_COMMENT_BATCH_SIZE));
    while ($comment = db_fetch_object($result)) {
      _feeds_comment_delete($comment->nid);
    }
    if (db_result(db_query_range("SELECT c.cid FROM {comment} c INNER JOIN {feeds_comment_item} fci ON c.cid = fci.nid WHERE fci.id = '%s' AND c.timestamp < %d", $this->id, FEEDS_REQUEST_TIME - $time, 0, 1))) {
      return FEEDS_BATCH_ACTIVE;
    }
    return FEEDS_BATCH_COMPLETE;
  }

  /**
   * Return expiry time.
   */
  public function expiryTime() {
    return $this->config['expire'];
  }

  /**
   * Override parent::configDefaults().
   */
  public function configDefaults() {
    return array(
      'input_format' => FILTER_FORMAT_DEFAULT,
      'update_existing' => FEEDS_SKIP_EXISTING,
      'expire' => FEEDS_EXPIRE_NEVER,
      'mappings' => array(),
      'author' => 0,
      'authorize' => 0,
    );
  }

  /**
   * Override parent::configForm().
   */
  public function configForm(&$form_state) {
    $form = array();
    $format_options = array(FILTER_FORMAT_DEFAULT => t('Default format'));
    $formats = filter_formats();
      foreach ($formats as $format) {
        $format_options[$format->format] = $format->name;
      }
    $form['input_format'] = array(
      '#type' => 'select',
      '#title' => t('Input format'),
      '#description' => t('Select the input format for the comments to be created.'),
      '#options' => $format_options,
      '#default_value' => $this->config['input_format'],
    );
    $author = user_load(array('uid' => $this->config['author']));
    $form['author'] = array(
      '#type' => 'textfield',
      '#title' => t('Author'),
      '#description' => t('Select the author of the comments to be created - leave empty to assign "anonymous".'),
      '#autocomplete_path' => 'user/autocomplete',
      '#default_value' => empty($author->name) ?  'anonymous' : check_plain($author->name),
    );
    $form['authorize'] = array(
      '#type' => 'checkbox',
      '#title' => t('Authorize'),
      '#description' => t('Check that the author has permission to create the node.'),
      '#default_value' => $this->config['authorize'],
    );
    $period = drupal_map_assoc(array(FEEDS_EXPIRE_NEVER, 3600, 10800, 21600, 43200, 86400, 259200, 604800, 604800 * 4, 604800 * 12, 604800 * 24, 31536000), 'feeds_format_expire');
    $form['expire'] = array(
      '#type' => 'select',
      '#title' => t('Expire comments'),
      '#options' => $period,
      '#description' => t('Select after how much time comments should be deleted. The comment\'s published date will be used for determining the comment\'s age, see Mapping settings.'),
      '#default_value' => $this->config['expire'],
    );
    $form['update_existing'] = array(
      '#type' => 'radios',
      '#title' => t('Update existing comments'),
      '#description' => t('Select how existing comments should be updated. Existing comments will be determined using mappings that are a "unique target".'),
      '#options' => array(
        FEEDS_SKIP_EXISTING => 'Do not update existing comments',
        FEEDS_REPLACE_EXISTING => 'Replace existing comments',
        FEEDS_UPDATE_EXISTING => 'Update existing comments (slower than replacing them)',
      ),
      '#default_value' => $this->config['update_existing'],
    );
    return $form;
  }

  /**
   * Override parent::configFormValidate().
   */
  public function configFormValidate(&$values) {
    if ($author = user_load(array('name' => $values['author']))) {
      $values['author'] = $author->uid;
    }
    else {
      $values['author'] = 0;
    }
  }

  /**
   * Override setTargetElement to operate on a target item that is a comment.
   */
  public function setTargetElement($target_comment, $target_element, $value) {
    if (in_array($target_element, array('guid'))) {
      $target_comment->feeds_comment_item->$target_element = $value;
    }
    elseif (array_key_exists($target_element, $this->getMappingTargets())) {
      $target_comment->$target_element = $value;
    }
  }

  /**
   * Return available mapping targets.
   */
  public function getMappingTargets() {
    $targets = array(
      'pid' => array(
        'name' => t('Parent ID'),
        'description' => t('The cid to which this comment is a reply.'),
       ),
      'nid' => array(
        'name' => t('Node ID'),
        'description' => t('The nid to which this comment is a reply.'),
      ),
      'uid' => array(
        'name' => t('User ID'),
        'description' => t('The Drupal user ID of the comment author.'),
      ),
      'subject' => array(
        'name' => t('Title'),
        'description' => t('The title of the comment.'),
       ),
      'comment' => array(
        'name' => t('Comment'),
        'description' => t('The comment body.'),
      ),
      'hostname' => array(
        'name' => t('Hostname'),
        'description' => t('The author\'s host name.'),
       ),
      'timestamp' => array(
        'name' => t('Published date'),
        'description' => t('The UNIX time when a comment has been saved.'),
      ),
      'status' => array(
        'name' => t('Published status'),
        'description' => t('The published status of a comment. (0 = Published, 1 = Not Published)'),
      ),
      'name' => array(
        'name' => t('Name'),
        'description' => t('The comment author\'s name.'),
      ),
      'mail' => array(
        'name' => t('Email'),
        'description' => t('The comment author\'s e-mail address.'),
      ),
      'homepage' => array(
        'name' => t('Homepage'),
        'description' => t('The comment author\'s home page address'),
      ),
      'guid' => array(
        'name' => t('GUID'),
        'description' => t('The external GUID of the comment. E. g. the feed item GUID in the case of a syndication feed. May be unique.'),
        'optional_unique' => TRUE,
      ),
    );

    // Let other modules expose mapping targets.
    self::loadMappers();
    drupal_alter('feeds_comment_processor_targets', $targets);

    return $targets;
  }

  /**
   * Get cid of an existing feed item comment if available.
   */
  protected function existingItemId($source_item, FeedsSource $source) {

    // Iterate through all unique targets and test whether they do already
    // exist in the database.
    foreach ($this->uniqueTargets($source_item) as $target => $value) {
      switch ($target) {
        case 'guid':
          $cid = db_result(db_query("SELECT cid FROM {feeds_comment_item} WHERE feed_nid = %d AND id = '%s' AND guid = '%s'", $source->feed_nid, $source->id, $value));
          break;
      }
      if ($cid) {
        // Return with the first nid found.
        return $cid;
      }
    }
    return 0;
  }

  /**
   * Creates a new comment object in memory and returns it.
   */
  protected function buildComment($cid, $feed_nid) {
    $comment = new stdClass();
    if (empty($cid)) {
      $comment->created = FEEDS_REQUEST_TIME;
      $populate = TRUE;
    }
    else {
      if ($this->config['update_existing'] == FEEDS_UPDATE_EXISTING) {
        $comment = _comment_load($cid);
      }
      else {
        $comment->cid = $cid;
        $populate = TRUE;
      }
    }
    if ($populate) {
      $comment->timestamp = FEEDS_REQUEST_TIME;
      $comment->format = $this->config['input_format'];
      $comment->feeds_comment_item = new stdClass();
      $comment->feeds_comment_item->id = $this->id;
      $comment->feeds_comment_item->imported = FEEDS_REQUEST_TIME;
      $comment->feeds_comment_item->feed_nid = $feed_nid;
      $comment->feeds_comment_item->guid = '';
      $comment->uid = $this->config['author'];
      $account = user_load(array('uid' => $comment->uid));
      $comment->name = $account->name;
      $comment->mail = $account->mail;
      $comment->status = 0;
      $comment->pid = 0;
      $comment->hostname = '127.0.0.1';

    }

    return $comment;
  }

  /**
   * Create MD5 hash of item and mappings array.
   *
   * Include mappings as a change in mappings may have an affect on the item
   * produced.
   *
   * @return Always returns a hash, even with empty, NULL, FALSE:
   *  Empty arrays return 40cd750bba9870f18aada2478b24840a
   *  Empty/NULL/FALSE strings return d41d8cd98f00b204e9800998ecf8427e
   */
  protected function hash($item) {
    static $serialized_mappings;
    if (!$serialized_mappings) {
      $serialized_mappings = serialize($this->config['mappings']);
    }
    return hash('md5', serialize($item) . $serialized_mappings);
  }

  /**
   * Retrieve MD5 hash of $cid from DB.
   * @return Empty string if no item is found, hash otherwise.
   */
  protected function getHash($cid) {
    $hash = db_result(db_query("SELECT hash FROM {feeds_comment_item} WHERE cid = %d", $cid));
    if ($hash) {
      // Return with the hash.
      return $hash;
    }
    return '';
  }
}

function _feeds_comment_delete($cid) {
  module_load_include('inc', 'comment', 'comment.admin');
  $to_delete = _comment_load($cid);
  // Delete comment and its replies.
  _comment_delete_thread($to_delete);
  _comment_update_node_statistics($to_delete->nid);
}

/**
 * This function is copied and pasted from comment_save in Drupal core and then
 * modified to get rid of access restrictions and to prevent extraneous dsms.
 *
 * @param $edit
 *   A comment array.
 *
 * @return
 *   If the comment is successfully saved the comment ID is returned. If the comment
 *   is not saved, FALSE is returned.
 */
function _feeds_comment_save($edit) {
  global $user;

  $edit += array(
    'mail' => '',
    'homepage' => '',
    'name' => '',
    'status' => user_access('post comments without approval') ? COMMENT_PUBLISHED : COMMENT_NOT_PUBLISHED,
  );
  if ($edit['cid']) {
    // Update the comment in the database.
    db_query("UPDATE {comments} SET status = %d, timestamp = %d, subject = '%s', comment = '%s', format = %d, uid = %d, name = '%s', mail = '%s', homepage = '%s' WHERE cid = %d", $edit['status'], $edit['timestamp'], $edit['subject'], $edit['comment'], $edit['format'], $edit['uid'], $edit['name'], $edit['mail'], $edit['homepage'], $edit['cid']);

    // Allow modules to respond to the updating of a comment.
    comment_invoke_comment($edit, 'update');

    // Add an entry to the watchdog log.
    watchdog('content', 'Comment: updated %subject.', array('%subject' => $edit['subject']), WATCHDOG_NOTICE, l(t('view'), 'node/'. $edit['nid'], array('fragment' => 'comment-'. $edit['cid'])));
  }
  else {
    // Add the comment to database.
    // Here we are building the thread field. See the documentation for
    // comment_render().
    if ($edit['pid'] == 0) {
      // This is a comment with no parent comment (depth 0): we start
      // by retrieving the maximum thread level.
      $max = db_result(db_query('SELECT MAX(thread) FROM {comments} WHERE nid = %d', $edit['nid']));

      // Strip the "/" from the end of the thread.
      $max = rtrim($max, '/');

      // Finally, build the thread field for this new comment.
      $thread = int2vancode(vancode2int($max) + 1) .'/';
    }
    else {
      // This is comment with a parent comment: we increase
      // the part of the thread value at the proper depth.

      // Get the parent comment:
      $parent = _comment_load($edit['pid']);

      // Strip the "/" from the end of the parent thread.
      $parent->thread = (string) rtrim((string) $parent->thread, '/');

      // Get the max value in _this_ thread.
      $max = db_result(db_query("SELECT MAX(thread) FROM {comments} WHERE thread LIKE '%s.%%' AND nid = %d", $parent->thread, $edit['nid']));

      if ($max == '') {
        // First child of this parent.
        $thread = $parent->thread .'.'. int2vancode(0) .'/';
      }
      else {
        // Strip the "/" at the end of the thread.
        $max = rtrim($max, '/');

        // We need to get the value at the correct depth.
        $parts = explode('.', $max);
        $parent_depth = count(explode('.', $parent->thread));
        $last = $parts[$parent_depth];

        // Finally, build the thread field for this new comment.
        $thread = $parent->thread .'.'. int2vancode(vancode2int($last) + 1) .'/';
      }
    }

    if (empty($edit['timestamp'])) {
      $edit['timestamp'] = time();
    }

    if ($edit['uid'] === $user->uid && isset($user->name)) { // '===' Need to modify anonymous users as well.
      $edit['name'] = $user->name;
    }

    db_query("INSERT INTO {comments} (nid, pid, uid, subject, comment, format, hostname, timestamp, status, thread, name, mail, homepage) VALUES (%d, %d, %d, '%s', '%s', %d, '%s', %d, %d, '%s', '%s', '%s', '%s')", $edit['nid'], $edit['pid'], $edit['uid'], $edit['subject'], $edit['comment'], $edit['format'], empty($edit['hostname']) ? ip_address() : $edit['hostname'], $edit['timestamp'], $edit['status'], $thread, $edit['name'], $edit['mail'], $edit['homepage']);
    $edit['cid'] = db_last_insert_id('comments', 'cid');

    // Tell the other modules a new comment has been submitted.
    comment_invoke_comment($edit, 'insert');

    // Add an entry to the watchdog log.
    watchdog('content', 'Comment: added %subject.', array('%subject' => $edit['subject']), WATCHDOG_NOTICE, l(t('view'), 'node/'. $edit['nid'], array('fragment' => 'comment-'. $edit['cid'])));
  }
  _comment_update_node_statistics($edit['nid']);

  // Clear the cache so an anonymous user can see his comment being added.
  cache_clear_all();

  // Explain the approval queue if necessary, and then
  // redirect the user to the node he's commenting on.
  if ($edit['status'] == COMMENT_NOT_PUBLISHED) {
    drupal_set_message(t('Your comment has been queued for moderation by site administrators and will be published after approval.'));
  }
  else {
    comment_invoke_comment($edit, 'publish');
  }
  return $edit['cid'];
}
