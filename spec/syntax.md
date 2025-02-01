# Syntax sketch

TODO: Write more english and less code.

Example syntax for this unnamed toy language, using an implementation for the
Sieve of Eratosthenes algorithm as reference:

```
class Sieve {
    fn run(let limit) {
        let prime[limit + 1];
        for (let i = 0; i <= limit; i += 1) {
            prime[i] = true;
        }

        prime[0] = prime[1] = false;

        for (let p = 2; p * p <= limit; p += 1) {
            if (prime[p]) {
                for (let i = p * p; i <= limit; i += p) {
                    prime[i] = false;
                }
            }
        }

        for (let i = 2; i <= limit; i += 1) {
            if (prime[i]) {
                puts(i);
            }
        }
        puts "\n";
    }
}

fn main() {
    let limit = 1000;

    let sieve = Sieve();

    puts "Prime numbers up to 1000: \n";
    sieve.run(limit);

    ret 0;
}
```
