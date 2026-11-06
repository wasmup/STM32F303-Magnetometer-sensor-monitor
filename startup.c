#include <stdint.h>

extern int main(void);
extern uint32_t _estack;

void Reset_Handler(void);
void Default_Handler(void);

__attribute__((section(".isr_vector"), used))
const uintptr_t vectors[] = {
    (uintptr_t)&_estack,
    (uintptr_t)Reset_Handler};

void Reset_Handler(void)
{
    main();
    for (;;)
    {
    }
}

void Default_Handler(void)
{
    for (;;)
    {
    }
}
