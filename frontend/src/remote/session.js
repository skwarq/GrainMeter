export function sessionSnapshot(data, now = Date.now()) {
  return {
    receivedAt: now,
    deadline:
      data.until > 0 ? data.until * 1000 : Number.isFinite(data.remaining_s) ? now + data.remaining_s * 1000 : null,
    budgetLeft: Number.isFinite(data.budget_left_s) ? data.budget_left_s : null,
  };
}

export function remainingSeconds(session, now = Date.now()) {
  return session?.deadline == null ? null : Math.max(0, Math.ceil((session.deadline - now) / 1000));
}

export function sessionTime(seconds) {
  return seconds == null ? '?' : Math.floor(seconds / 60) + ':' + String(seconds % 60).padStart(2, '0');
}
