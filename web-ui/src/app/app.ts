import { Component, OnInit, inject, signal } from '@angular/core';
import { ApiService } from './core/api.service';
import { DashboardComponent } from './dashboard/dashboard';
import { StreamComponent } from './stream/stream';

type Tab = 'stream' | 'dashboard';

@Component({
  imports: [StreamComponent, DashboardComponent],
  selector: 'app-root',
  styleUrl: './app.css',
  templateUrl: './app.html',
})
export class App implements OnInit {
  protected readonly api = inject(ApiService);
  protected readonly tab = signal<Tab>('stream');
  protected readonly tabs: { id: Tab; label: string }[] = [
    { id: 'stream', label: 'Packets' },
    { id: 'dashboard', label: 'Dashboard' },
  ];

  ngOnInit(): void {
    this.api.connect();
  }
}
