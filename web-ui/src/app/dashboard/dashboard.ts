import { Component, computed, inject } from '@angular/core';
import { ApiService } from '../core/api.service';
import { formatTime } from '../core/hexdump';

@Component({
  selector: 'app-dashboard',
  templateUrl: './dashboard.html',
  styleUrl: './dashboard.css',
})
export class DashboardComponent {
  protected readonly api = inject(ApiService);
  protected readonly time = formatTime;
  protected readonly s = this.api.status;
  protected readonly uptime = computed(() => {
    const sec = this.s()?.uptime_s ?? 0;
    const h = Math.floor(sec / 3600);
    const m = Math.floor((sec % 3600) / 60);
    return `${h}h ${m}m ${sec % 60}s`;
  });

  protected kb(bytes: number): string {
    return bytes < 1024 ? `${bytes} B` : bytes < 1048576 ? `${(bytes / 1024).toFixed(1)} KiB` : `${(bytes / 1048576).toFixed(1)} MiB`;
  }
}
