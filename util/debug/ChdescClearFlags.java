import java.io.DataInput;
import java.io.IOException;

public class ChdescClearFlags extends Opcode
{
	private final int chdesc, flags;
	
	public ChdescClearFlags(int chdesc, int flags)
	{
		this.chdesc = chdesc;
		this.flags = flags;
	}
	
	public void applyTo(SystemState state)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_CLEAR_FLAGS, "KDB_CHDESC_CLEAR_FLAGS", ChdescClearFlags.class);
		factory.addParameter("chdesc", 4);
		factory.addParameter("flags", 4);
		return factory;
	}
}
