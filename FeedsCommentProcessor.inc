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
   * Define entity type.
   */
  public function entityType() {
    return 'user';
  }

  /**
   * Implements parent::entityInfo().
   */
  protected function entityInfo() {
    $info = parent::entityInfo();
    $info['label plural'] = t('Comments');
    return $info;
  }

  /**
   * Creates a new comment in memory and returns it.
   */
  protected function newEntity(FeedsSource $source) {
    $comment = new stdClass();
    $comment->created = FEEDS_REQUEST_TIME;
    $comment->timestamp = FEEDS_REQUEST_TIME;
    $comment->format = $this->config['input_format'];
    $comment->feeds_comment_item = new stdClass();
    $comment->feeds_comment_item->id = $this->id;
    $comment->feeds_comment_item->imported = FEEDS_REQUEST_TIME;
    $comment->feeds_comment_item->feed_nid = $source->feed_nid;
    $comment->feeds_comment_item->guid = '';
    $comment->uid = $this->config['author'];
    $account = user_load($comment->uid);
    $comment->name = $account->name;
    $comment->mail = $account->mail;
    $comment->status = 0;
    $comment->pid = 0;
    $comment->hostname = '127.0.0.1';
    return $comment;
  }

  /**
   * Loads an existing comment.
   */
  protected function entityLoad(FeedsSource $source, $cid) {
    return comment_load($cid);
  }

  /**
   * Validates a comment.
   */
  protected function entityValidate($comment) {
    if (empty($comment->nid)) {
      throw new FeedsValidationException(t('Unable create comment with empty NID.'));
    }
  }

  /**
   * Save a comment.
   */
  public function entitySave($comment) {
    _feeds_comment_save((array)$comment);
  }

  /**
   * Delete multiple comments.
   */
  protected function entityDeleteMultiple($cids) {
    foreach ($cids as $cid) {
      _feeds_comment_delete($cid);
    }
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
    $result = db_query_range("SELECT c.cid FROM {comments} c INNER JOIN {feeds_comment_item} fci ON c.cid = fni.nid WHERE fci.id = :id AND c.timestamp < :timestamp", 0, variable_get('feeds_comment_batch_size', FEEDS_COMMENT_BATCH_SIZE), array(':id' => $this->id, ':timestamp' => FEEDS_REQUEST_TIME - $time));
    while ($comment = db_fetch_object($result)) {
      _feeds_comment_delete($comment->nid);
    }
    if (db_result(db_query_range("SELECT c.cid FROM {comment} c INNER JOIN {feeds_comment_item} fci ON c.cid = fci.nid WHERE fci.id = :id AND c.timestamp < :timestamp", 0, 1, array(':id' => $this->id, ':timestamp' => FEEDS_REQUEST_TIME - $time)))) {
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
      'input_format' => filter_default_format(),
      'update_existing' => FEEDS_SKIP_EXISTING,
      'expire' => FEEDS_EXPIRE_NEVER,
      'mappings' => array(),
      'author' => 0,
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
    $author = user_load($this->config['author']);
    $form['author'] = array(
      '#type' => 'textfield',
      '#title' => t('Author'),
      '#description' => t('Select the author of the comments to be created - leave empty to assign "anonymous".'),
      '#autocomplete_path' => 'user/autocomplete',
      '#default_value' => empty($author->name) ?  'anonymous' : check_plain($author->name),
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
    if ($author = user_load_by_name($values['author'])) {
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
  protected function existingEntityId(FeedsSource $source, FeedsParserResult $result) {
    if ($cid = parent::existingEntityId($source, $result)) {
      return $cid;
    }

    // Iterate through all unique targets and test whether they do already
    // exist in the database.
    foreach ($this->uniqueTargets($source, $result) as $target => $value) {
      switch ($target) {
        case 'guid':
          $cid = db_query("SELECT cid FROM {feeds_comment_item} WHERE feed_nid = :feed_nid AND id = :id AND guid = :guid", array(':feed_nid' => $source->feed_nid, ':id' => $source->id, ':guid' => $value))->fetchField();
          break;
      }
      if ($cid) {
        // Return with the first nid found.
        return $cid;
      }
    }
    return 0;
  }
}

function _feeds_comment_delete($cid) {
  module_load_include('inc', 'comment', 'comment.admin');
  $to_delete = comment_load($cid);
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
    db_update('comments')
  ->fields(array(
      'status' => $edit['status'],
      'timestamp' => $edit['timestamp'],
      'subject' => $edit['subject'],
      'comment' => $edit['comment'],
      'format' => $edit['format'],
      'uid' => $edit['uid'],
      'name' => $edit['name'],
      'mail' => $edit['mail'],
      'homepage' => $edit['homepage'],
    ))
  ->condition('cid', $edit['cid'])
  ->execute();

    // Allow modules to respond to the updating of a comment.
    comment_invoke_comment($edit, 'update');

    // Add an entry to the watchdog log.
    watchdog('content', 'Comment: updated %subject.', array('%subject' => $edit['subject']), WATCHDOG_NOTICE, l(t('view'), 'node/' . $edit['nid'], array('fragment' => 'comment-' . $edit['cid'])));
  }
  else {
    // Add the comment to database.
    // Here we are building the thread field. See the documentation for
    // comment_render().
    if ($edit['pid'] == 0) {
      // This is a comment with no parent comment (depth 0): we start
      // by retrieving the maximum thread level.
      $max = db_query('SELECT MAX(thread) FROM {comments} WHERE nid = :nid', array(':nid' => $edit['nid']))->fetchField();

      // Strip the "/" from the end of the thread.
      $max = rtrim($max, '/');

      // Finally, build the thread field for this new comment.
      $thread = int2vancode(vancode2int($max) + 1) . '/';
    }
    else {
      // This is comment with a parent comment: we increase
      // the part of the thread value at the proper depth.

      // Get the parent comment:
      $parent = comment_load($edit['pid']);

      // Strip the "/" from the end of the parent thread.
      $parent->thread = (string) rtrim((string) $parent->thread, '/');

      // Get the max value in _this_ thread.
      $max = db_query("SELECT MAX(thread) FROM {comments} WHERE thread LIKE '%s.%%' AND nid = :nid", array(':nid' => $parent->thread, '' => $edit['nid']))->fetchField();

      if ($max == '') {
        // First child of this parent.
        $thread = $parent->thread . '.' . int2vancode(0) . '/';
      }
      else {
        // Strip the "/" at the end of the thread.
        $max = rtrim($max, '/');

        // We need to get the value at the correct depth.
        $parts = explode('.', $max);
        $parent_depth = count(explode('.', $parent->thread));
        $last = $parts[$parent_depth];

        // Finally, build the thread field for this new comment.
        $thread = $parent->thread . '.' . int2vancode(vancode2int($last) + 1) . '/';
      }
    }

    if (empty($edit['timestamp'])) {
      $edit['timestamp'] = REQUEST_TIME;
    }

    if ($edit['uid'] === $user->uid && isset($user->name)) { // '===' Need to modify anonymous users as well.
      $edit['name'] = $user->name;
    }

    $id = db_insert('comments')
  ->fields(array(
      'nid' => $edit['nid'],
      'pid' => $edit['pid'],
      'uid' => $edit['uid'],
      'subject' => $edit['subject'],
      'comment' => $edit['comment'],
      'format' => $edit['format'],
      'hostname' => empty($edit['hostname']) ? ip_address() : $edit['hostname'],
      'timestamp' => $edit['timestamp'],
      'status' => $edit['status'],
      'thread' => $thread,
      'name' => $edit['name'],
      'mail' => $edit['mail'],
      'homepage' => $edit['homepage'],
    ))
  ->execute();
    $edit['cid'] = db_last_insert_id('comments', 'cid');

    // Tell the other modules a new comment has been submitted.
    comment_invoke_comment($edit, 'insert');

    // Add an entry to the watchdog log.
    watchdog('content', 'Comment: added %subject.', array('%subject' => $edit['subject']), WATCHDOG_NOTICE, l(t('view'), 'node/' . $edit['nid'], array('fragment' => 'comment-' . $edit['cid'])));
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
