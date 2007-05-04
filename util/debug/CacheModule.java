import java.io.IOException;

public class CacheModule extends Module
{
	public CacheModule(CountingDataInput input) throws BadInputException, IOException
	{
		super(input, KDB_MODULE_CACHE);
		
		addFactory(CacheNotify.getFactory(input));
		addFactory(CacheFindBlock.getFactory(input));
		addFactory(CacheLookBlock.getFactory(input));
		addFactory(CacheWriteBlock.getFactory(input));
	}
}
