fn main() {
  let mut x = 1;
  add_two(&mut x);
  assert!(x == 3);
}
fn add_two(x : *mut u32) {
  unsafe {
    *x += 2;
  }
}