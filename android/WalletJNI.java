// Copyright 2018 The Beam Team
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import com.mw.beam.beamwallet.core.*;
import com.mw.beam.beamwallet.core.entities.*;

public class WalletJNI
{
	public static void main(String[] args)
	{
		System.out.println("Start Wallet JNI test...");

		Api api = new Api();

		Wallet wallet;

		String nodeAddr = "172.104.249.212:8101";

		if(api.isWalletInitialized("test"))
		{
			wallet = api.openWallet(nodeAddr, "test", "123");

			System.out.println(wallet == null ? "wallet opening error" : "wallet successfully opened");
		}
		else
		{
			wallet = api.createWallet(nodeAddr, "test", "123", "000");

			System.out.println(wallet == null ? "wallet creation error" : "wallet successfully created");
		}

		wallet.syncWithNode();

		while(true)
		{
			System.out.println("Show info about wallet.");

			// call async wallet requests
			{
				wallet.getWalletStatus();
				wallet.getUtxosStatus();
			}
			try
			{
				Thread.sleep(5000);
			}
			catch(InterruptedException e) {}
		}
	}
}