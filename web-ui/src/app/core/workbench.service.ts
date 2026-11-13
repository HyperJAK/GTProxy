import { Injectable, signal } from '@angular/core';
import { Dir } from './models';
import { PacketSpec } from './spec';

export type Tab = 'stream' | 'intercept' | 'inject' | 'plugins' | 'settings' | 'dashboard';

/** Cross-view state: current tab and a packet handed from the stream to the injector. */
@Injectable({ providedIn: 'root' })
export class Workbench {
  readonly tab = signal<Tab>('stream');
  readonly draft = signal<{ spec: PacketSpec; dir: Dir } | null>(null);

  sendToInjector(spec: PacketSpec, dir: Dir): void {
    this.draft.set({ spec, dir });
    this.tab.set('inject');
  }
}
