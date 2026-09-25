//! Уведомления пользователю. Как в исходном плагине: одно и то же сообщение не
//! повторяется подряд, иначе ошибка в конфиге заспамила бы экран на каждый кадр.

use std::collections::VecDeque;

#[derive(Debug, Default)]
pub struct Notifier {
    last: Option<String>,
    queue: VecDeque<String>,
}

impl Notifier {
    pub fn push(&mut self, msg: impl Into<String>) {
        let msg = msg.into();
        if self.last.as_deref() == Some(msg.as_str()) {
            return;
        }
        self.last = Some(msg.clone());
        // очередь не растёт бесконечно, если прослойка почему-то не забирает сообщения
        if self.queue.len() >= 16 {
            self.queue.pop_front();
        }
        self.queue.push_back(msg);
    }

    /// После смены конфига прошлые ошибки снова можно показать.
    pub fn reset(&mut self) {
        self.last = None;
    }

    pub fn pop(&mut self) -> Option<String> {
        self.queue.pop_front()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn dedups_consecutive() {
        let mut n = Notifier::default();
        n.push("a");
        n.push("a");
        n.push("b");
        n.push("a");
        assert_eq!(n.pop().as_deref(), Some("a"));
        assert_eq!(n.pop().as_deref(), Some("b"));
        assert_eq!(n.pop().as_deref(), Some("a"));
        assert_eq!(n.pop(), None);
    }

    #[test]
    fn reset_allows_repeat() {
        let mut n = Notifier::default();
        n.push("a");
        n.reset();
        n.push("a");
        assert_eq!(n.pop().as_deref(), Some("a"));
        assert_eq!(n.pop().as_deref(), Some("a"));
    }

    #[test]
    fn queue_is_bounded() {
        let mut n = Notifier::default();
        for i in 0..100 {
            n.push(i.to_string());
        }
        assert_eq!(std::iter::from_fn(|| n.pop()).count(), 16);
    }
}
