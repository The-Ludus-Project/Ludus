"""Editor layout metadata policy, shared with the generated private Qt header."""

import re

MAX_COORDINATE = 32768
MAX_COMMENT_FRAMES = 64
MAX_COMMENT_TITLE_CODEPOINTS = 128
MAX_COMMENT_NOTES_CODEPOINTS = 4096
MIN_COMMENT_WIDTH = 160
MIN_COMMENT_HEIGHT = 100


def validate_frames(frames, location, graph):
    graph._list(frames, MAX_COMMENT_FRAMES, location)
    claimed = set()
    for frame in frames:
        graph._keys(frame, ('id', 'title', 'notes', 'colour', 'x', 'y', 'width', 'height'), location)
        graph._identifier(frame['id'], location)
        graph._require(frame['id'] not in claimed, 'Schema', location, 'duplicate comment frame')
        claimed.add(frame['id'])
        graph._require(type(frame['title']) is str and frame['title'].strip()
                       and len(frame['title']) <= MAX_COMMENT_TITLE_CODEPOINTS
                       and '\n' not in frame['title'] and '\r' not in frame['title'],
                       'Schema', location, 'bounded nonempty comment title required')
        graph._require(type(frame['notes']) is str and len(frame['notes']) <= MAX_COMMENT_NOTES_CODEPOINTS,
                       'Schema', location, 'bounded comment notes required')
        graph._require(type(frame['colour']) is str and re.fullmatch(r'#[0-9a-fA-F]{6}', frame['colour']),
                       'Schema', location, 'comment colour must be #RRGGBB')
        for coordinate in ('x', 'y'):
            graph._integer(frame[coordinate], -MAX_COORDINATE, MAX_COORDINATE, location)
        graph._integer(frame['width'], MIN_COMMENT_WIDTH, 2 * MAX_COORDINATE, location)
        graph._integer(frame['height'], MIN_COMMENT_HEIGHT, 2 * MAX_COORDINATE, location)
        graph._require(frame['x'] + frame['width'] <= MAX_COORDINATE
                       and frame['y'] + frame['height'] <= MAX_COORDINATE,
                       'Schema', location, 'comment frame exceeds canvas coordinate bounds')
