from codex_buddy.activity_heartbeat import ActivityHeartbeat


def test_activity_heartbeat_projects_events_into_twenty_one_second_buckets():
    heartbeat = ActivityHeartbeat()

    heartbeat.record(100.2)
    assert heartbeat.mask(100.9) == 0b1
    assert heartbeat.mask(101.0) == 0b10

    heartbeat.record(101.4)
    assert heartbeat.mask(101.9) == 0b11
    assert heartbeat.mask(119.0) == (0b11 << 18)
    assert heartbeat.mask(120.0) == (1 << 19)
    assert heartbeat.mask(121.0) == 0


def test_activity_heartbeat_ignores_future_and_expired_events():
    heartbeat = ActivityHeartbeat()
    heartbeat.record(50.0)
    heartbeat.record(80.0)

    assert heartbeat.mask(70.0) == 0
    assert heartbeat.mask(100.0) == 0
