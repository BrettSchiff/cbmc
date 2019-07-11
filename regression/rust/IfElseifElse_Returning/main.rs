fn main() {
  let a : u32;

  let b =
  if a % 3 == 0 {
    assert!(a != 5);
    0
  }
  else if a % 3 == 1 {
    assert!(a > 0);
    1
  }
  else {
    assert!(a > 1);
    2
  };

  assert!(b < 3);

  let c : u32;

  let d =
  if c > 100 {
    c
  } else if c < 10 {
    c + 101
  } else {
    c * 11
  };

  assert!(d > 100);
}