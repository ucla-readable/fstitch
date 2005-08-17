import java.io.DataInput;
import java.io.IOException;

public class ChdescSetFreePrev extends Opcode
{
	private final int chdesc, free_prev;
	
	public ChdescSetFreePrev(int chdesc, int free_prev)
	{
		this.chdesc = chdesc;
		this.free_prev = free_prev;
	}
	
	public void applyTo(SystemState state)
	{
		Chdesc chdesc = state.lookupChdesc(this.chdesc);
		if(chdesc != null)
		{
			if(free_prev == 0)
				chdesc.setFreePrev(null);
			else
			{
				Chdesc free_prev = state.lookupChdesc(this.free_prev);
				if(free_prev == null)
				{
					free_prev = new Chdesc(this.free_prev);
					/* should we really do this? */
					state.addChdesc(free_prev);
				}
				chdesc.setFreePrev(free_prev);
			}
		}
	}
	
	public boolean hasEffect()
	{
		return true;
	}
	
	public String toString()
	{
		return "KDB_CHDESC_SET_FREE_PREV: chdesc = " + SystemState.hex(chdesc) + ", free_prev = " + SystemState.hex(free_prev);
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_SET_FREE_PREV, "KDB_CHDESC_SET_FREE_PREV", ChdescSetFreePrev.class);
		factory.addParameter("chdesc", 4);
		factory.addParameter("free_prev", 4);
		return factory;
	}
}
