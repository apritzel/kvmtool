#ifndef KVM__KVM_CPU_H
#define KVM__KVM_CPU_H

#include "kvm/kvm-cpu-arch.h"

struct kvm_cpu *kvm_cpu__init(struct kvm *kvm, unsigned long cpu_id);
void kvm_cpu__delete(struct kvm_cpu *vcpu);
void kvm_cpu__reset_vcpu(struct kvm_cpu *vcpu);
void kvm_cpu__setup_cpuid(struct kvm_cpu *vcpu);
void kvm_cpu__enable_singlestep(struct kvm_cpu *vcpu);
void kvm_cpu__run(struct kvm_cpu *vcpu);
void kvm_cpu__reboot(void);
int kvm_cpu__start(struct kvm_cpu *cpu);

int kvm_cpu__get_debug_fd(void);
void kvm_cpu__set_debug_fd(int fd);
void kvm_cpu__show_code(struct kvm_cpu *vcpu);
void kvm_cpu__show_registers(struct kvm_cpu *vcpu);
void kvm_cpu__show_page_tables(struct kvm_cpu *vcpu);

#endif /* KVM__KVM_CPU_H */
