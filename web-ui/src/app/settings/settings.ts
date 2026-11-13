import { Component, effect, inject, signal, untracked } from '@angular/core';
import { ApiService } from '../core/api.service';
import { Profile } from '../core/models';

interface SessionInfo {
  name: string;
  bytes: number;
}

interface ReplayResult {
  name: string;
  total: number;
  dropped: number;
  changed: number;
  skipped_lines: number;
  packets: { id: number; dir: string; dropped: boolean; changed: boolean; in_hex: string; outputs: { dir: string; hex: string }[] }[];
}

const BLANK: Profile = { name: '', address: '127.0.0.1', port: 443, tls: true, direct: false, enet_address: '127.0.0.1', enet_port: 17091 };

@Component({
  selector: 'app-settings',
  templateUrl: './settings.html',
  styleUrl: './settings.css',
})
export class SettingsComponent {
  protected readonly api = inject(ApiService);
  protected readonly profiles = signal<Profile[]>([]);
  protected readonly active = signal('');
  protected readonly draft = signal<Profile>({ ...BLANK });
  protected readonly sessions = signal<SessionInfo[]>([]);
  protected readonly recordName = signal('');
  protected readonly replay = signal<ReplayResult | null>(null);
  protected readonly message = signal<{ ok: boolean; text: string } | null>(null);
  protected readonly wsUrl = signal(this.api.url());

  constructor() {
    effect(() => {
      if (this.api.connected()) untracked(() => void this.refresh());
    });
  }

  protected async refresh(): Promise<void> {
    await this.run(async () => {
      const p = await this.api.call<{ active: string; profiles: Profile[] }>('profiles.list');
      this.profiles.set(p.profiles);
      this.active.set(p.active);
      this.sessions.set(await this.api.call<SessionInfo[]>('sessions.list'));
    }, false);
  }

  protected edit(p: Profile): void {
    this.draft.set({ ...p });
  }

  protected patch(p: Partial<Profile>): void {
    this.draft.set({ ...this.draft(), ...p });
  }

  protected async saveProfile(): Promise<void> {
    await this.run(async () => {
      const r = await this.api.call<{ active: string; profiles: Profile[] }>('profiles.save', this.draft());
      this.profiles.set(r.profiles);
      this.message.set({ ok: true, text: `Saved profile "${this.draft().name}".` });
    });
  }

  protected async activate(name: string): Promise<void> {
    await this.run(async () => {
      const r = await this.api.call<{ active: string; profiles: Profile[] }>('profiles.set_active', { name });
      this.active.set(r.active);
      this.message.set({ ok: true, text: `Active profile: ${name}. It applies to the next client connection.` });
    });
  }

  protected async deleteProfile(name: string): Promise<void> {
    await this.run(async () => {
      const r = await this.api.call<{ active: string; profiles: Profile[] }>('profiles.delete', { name });
      this.profiles.set(r.profiles);
    });
  }

  protected async startRecording(): Promise<void> {
    await this.run(async () => {
      await this.api.call('sessions.start', { name: this.recordName().trim() });
      this.message.set({ ok: true, text: 'Recording started.' });
      await this.refresh();
    });
  }

  protected async stopRecording(): Promise<void> {
    await this.run(async () => {
      await this.api.call('sessions.stop');
      this.message.set({ ok: true, text: 'Recording stopped.' });
      await this.refresh();
    });
  }

  protected async replaySession(name: string): Promise<void> {
    await this.run(async () => {
      this.replay.set(await this.api.call<ReplayResult>('sessions.replay', { name }, 30000));
    });
  }

  protected saveUrl(): void {
    this.api.setUrl(this.wsUrl());
  }

  private async run(fn: () => Promise<void>, report = true): Promise<void> {
    try {
      await fn();
    } catch (e) {
      if (report) this.message.set({ ok: false, text: (e as Error).message });
    }
  }
}
